// SPDX-FileCopyrightText: 2026 Kamayu B.V. <https://www.kamayu.nl>
// SPDX-License-Identifier: GPL-2.0-or-later

//=======================================================================
//
//  WhisperFilter.cpp
//
//  Created: 20 sep 2026
//
//  Author: N.H. Elroy with support from Claude Code (Sonnet)
//
//=======================================================================

#include "WhisperFilter.hpp"
#include "FilterOrigin.hpp"
#include "FilterProperties.hpp"
#include "ModelDownloadDialog.hpp" // runOnUiThreadLater
#include "ServerLink.hpp"
#include "SharedSettings.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

#include <obs-frontend-api.h>
#include <obs-module.h>

#ifdef min
#undef min
#endif

/*
    Finds any source on the canvas by name and sets its "text" property.  Safe to
    call from any thread. Takes the source NAME rather than the filter struct: callers
    on the whisper worker thread pass a snapshot taken under stateMtx, so this never
    reads f->textSourceName concurrently with a filterUpdate() write on the UI thread.
 */
void updateTextSource(const std::string &sourceName, const std::string &text)
{
	if (sourceName.empty())
		return;

	obs_source_t *source = obs_get_source_by_name(sourceName.c_str());
	if (!source) {
		blog(LOG_WARNING,
		     "[babelstreamer-filter] Text source '%s' not found - "
		     "check the source name in the filter properties",
		     sourceName.c_str());
		return;
	}

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "text", text.c_str());
	obs_source_update(source, settings);
	obs_data_release(settings);
	obs_source_release(source);

	blog(LOG_INFO, "[babelstreamer-filter] Caption → '%s': %s", sourceName.c_str(), text.c_str());
}

// Snapshot of this filter's contribution to the shared connection.
ServerLinkParams makeLinkParams(WhisperFilter *myFilter)
{
	ServerLinkParams params;
	{
		std::lock_guard<std::mutex> lock(myFilter->stateMtx);
		params.host = myFilter->wsHost;
		params.port = myFilter->wsPort;
		params.useTls = myFilter->wsUseTls;
		params.deviceToken = myFilter->deviceToken;
	}
	params.streamGateEnabled = myFilter->streamGateEnabled.load();
	params.source = myFilter->context;
	params.clearOverlay = [myFilter] {
		std::string overlayName;
		{
			std::lock_guard<std::mutex> lk(myFilter->stateMtx);
			overlayName = myFilter->textSourceName;
		}
		updateTextSource(overlayName, "");
	};
	return params;
}

/*
    Deal with OBS filter-enablement change event. Signal is set up so that
    data is a pointer to the filter
 */
static void onFilterEnableChanged(void *data, calldata_t *callData)
{
	auto *filter = static_cast<WhisperFilter *>(data);
	const bool enabled = calldata_bool(callData, "enabled");
	blog(LOG_INFO, "[babelstreamer-filter] Filter %s", enabled ? "enabled" : "disabled");

	/*
        Watch out for the fact that there may be multiple instances sharing resources
        serverLinkSetActive handles this with reference counting, but the overlay
        needs attention here.
     */
	ServerLink::getInstance().serverLinkSetActive(filter->linkId, enabled);
	if (!enabled) {
		std::string overlayName;
		{
			std::lock_guard<std::mutex> lock(filter->stateMtx);
			overlayName = filter->textSourceName;
		}
		updateTextSource(overlayName, "");
	}
}

/*
    Filter create function. Not invoked directly but passed as a parameter
    to OBS. When a filter is created it needs to be registered on the
    list of filters before it does anything else.
 */
static void *filterCreate(obs_data_t *settings, obs_source_t *source)
{
	auto *filter = new WhisperFilter();
	filter->context = source;

	registerFilter(filter);

	// tell the link handler what the streaming status is
	ServerLink::getInstance().setIsStreaming(obs_frontend_streaming_active());
	filterUpdate(filter, settings);

	/*
        If we don't have a model defined than ask for one, otherwise we may need to ask
        to link to the account. Only the master filter can meaningfully choose a
        model - a follower with no model yet just means the master hasn't set
        one, which the master's own filterCreate/filterUpdate will handle.

        isMasterFilterSource() is unreliable here, though: OBS doesn't attach
        a filter to its parent's filter chain (obs_source_filter_add()) until
        AFTER create() returns, so at this point in filterCreate() this filter
        is structurally invisible to OBS's own enumeration - it looks like a
        follower even if it's about to become (or is) the master, e.g. the
        very first filter on a brand new install. That would silently skip
        the model-download prompt with no error. Re-run this decision one
        tick later via runOnUiThreadLater(), once OBS has finished attaching
        it, so isMasterFilterSource() sees the truth. obs_source_update()
        re-runs filterUpdate() first so any shared-setting reconciliation
        (SharedSettings.hpp's reconcileShared) that was skipped the first
        time round for the same reason also gets a correct pass.
     */
	if (filter->modelPath.empty() && isMasterFilterSource(filter->context))
		offerModelDownload(filter);
	else
		maybeOfferAccountLink();

	runOnUiThreadLater([filter] {
		if (!filterStillRegistered(filter))
			return; // destroyed before this ran
		obs_source_update(filter->context, nullptr);
		if (filter->modelPath.empty() && isMasterFilterSource(filter->context))
			offerModelDownload(filter);
		else
			maybeOfferAccountLink();
	});

	// clear out leftover text
	updateTextSource(filter->textSourceName, "");

	// Join the shared server connection - the returned id identifies this filter to
	// the server handler
	filter->linkId = ServerLink::getInstance().serverLinkAdd(makeLinkParams(filter));

	// set the handler for enablement status change from OBS
	signal_handler_connect(obs_source_get_signal_handler(source), "enable", onFilterEnableChanged, filter);

	return filter;
}

static void filterDestroy(void *data)
{
	auto *filter = static_cast<WhisperFilter *>(data);
	unregisterFilter(filter);
	signal_handler_disconnect(obs_source_get_signal_handler(filter->context), "enable", onFilterEnableChanged,
				  filter);

	// Stop first so we don't accidentally queue after teardown
	{
		std::lock_guard<std::mutex> lockk(filter->whisperMtx);
		filter->whisper.reset();
	}

	// Unregister from the link
	ServerLink::getInstance().serverLinkRemove(filter->linkId);

	updateTextSource(filter->textSourceName, "");
	delete filter;
}

// Handle OBS audio data block - return unmodified because we only use it for transcript
static struct obs_audio_data *filterAudio(void *data, struct obs_audio_data *audio)
{
	auto *filter = static_cast<WhisperFilter *>(data);

	// Try to acquire the lock without blocking the audio thread.
	// Dropping a frame is better than stalling
	if (!filter->whisperMtx.try_lock())
		return audio;

	if (!filter->whisper) {
		filter->whisperMtx.unlock();
		return audio;
	}

	// Apply the stream gating
	if (filter->streamGateEnabled.load() && !ServerLink::getInstance().serverLinkIsStreaming()) {
		filter->whisperMtx.unlock();
		return audio;
	}

	// Determine source audio format
	const audio_output_info *aoi = audio_output_get_info(obs_get_audio());
	uint32_t srcRate = aoi ? aoi->samples_per_sec : 48000u;
	uint32_t srcChannels = aoi ? (uint32_t)get_audio_channels(aoi->speakers) : 2u;
	uint32_t nFrames = audio->frames;

	if (nFrames == 0) {
		filter->whisperMtx.unlock();
		return audio;
	}

	const float *const *planes = reinterpret_cast<const float *const *>(audio->data);

	// little helper to downmix a planar audio frame to mono
	auto monoSample = [&](uint32_t frame) -> float {
		if (srcChannels == 1)
			return planes[0][frame];
		double sum = 0.0;
		for (uint32_t c = 0; c < srcChannels; ++c)
			sum += planes[c][frame];
		return (float)(sum / srcChannels);
	};

	// resample to 16kHz
	const float ratio = (float)srcRate / (float)WHISPER_SAMPLE_RATE;

	std::vector<float> resampled;
	resampled.reserve((uint32_t)(nFrames / ratio) + 2);

	float pos = filter->resampleAccum;
	while (pos < (float)nFrames) {
		uint32_t i0 = (uint32_t)pos;
		uint32_t i1 = std::min(i0 + 1, nFrames - 1);
		float frac = pos - (float)i0;
		float s0 = monoSample(i0);
		float s1 = monoSample(i1);
		resampled.push_back(s0 * (1.0f - frac) + s1 * frac);
		pos += ratio;
	}
	// Carry fractional position into the next callback
	filter->resampleAccum = pos - (float)nFrames;

	if (!resampled.empty())
		filter->whisper->pushAudio16k(resampled.data(), (int)resampled.size());

	filter->whisperMtx.unlock();
	return audio; // pass audio through unmodified
}

// register the filter
static obs_source_info whisperFilterInfo = {};
static const char *filterGetName(void *)
{
	return "BabelStreamer Speech Filter";
}

// return the setup data for the filter
obs_source_info *getWhisperFilterInfo()
{
	whisperFilterInfo.id = "whisper_ws_filter";
	whisperFilterInfo.type = OBS_SOURCE_TYPE_FILTER;
	whisperFilterInfo.output_flags = OBS_SOURCE_AUDIO;
	whisperFilterInfo.get_name = filterGetName;
	whisperFilterInfo.create = filterCreate;
	whisperFilterInfo.destroy = filterDestroy;
	whisperFilterInfo.update = filterUpdate;
	whisperFilterInfo.get_properties = filterProperties;
	whisperFilterInfo.get_defaults = filterDefaults;
	whisperFilterInfo.filter_audio = filterAudio;
	return &whisperFilterInfo;
}
