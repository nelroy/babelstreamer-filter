// SPDX-FileCopyrightText: 2026 Kamayu B.V. <https://www.kamayu.nl>
// SPDX-License-Identifier: GPL-2.0-or-later

//=======================================================================
//
//  FilterProperties.cpp
//
//  Created: 26 sep 2026
//
//  Author: N.H. Elroy with support from Claude Code (Sonnet)
//
//  Dev Note: this could probably be refactored into something cleaner
//            but there is no real way to avoid the nitty gritty of fields
//            and parameters. There may be a better way to handle
//            shared resources, but that's for later.
//
//            Because many blocking operations here are trigged by an
//            OBS callback, blocking code needs to run in a different
//            context. Because there are multiple filters sharing the
//            same models we have to account for actions from other
//            filters, and be aware that the status may not be the same
//            as the moment that the function was queued.
//
//=======================================================================

#include "FilterProperties.hpp"
#include "BrandedPopup.hpp"
#include "DeviceLinkDialog.hpp"
#include "FilterOrigin.hpp"
#include "GameVocab.hpp"
#include "ModelDownloadDialog.hpp"
#include "ServerLink.hpp"
#include "SharedSettings.hpp"
#include "WhisperEngine.hpp"
#include "WhisperFilter.hpp"

#include <QDesktopServices>
#include <QUrl>

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <utility>

// Flag to say "you're allowed to do OBS stuff".  Will be set when
// scenes are finished creating and unset when scenes are changed
static std::atomic<bool> frontendReadyFlag{false};

// Trigger to deal with flag - is armed further down the module. This is also
// where we chack if the account is connected
static void frontendReadyEvent(enum obs_frontend_event event, void *)
{
    if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING || event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED)
    {
        frontendReadyFlag.store(true);
        /*
            Offer to link on account on every plugin start, not just when someone presses the button.
            If OBS was never connected it looks fine right up until the first transcript is silently dropped.
         */
        maybeOfferAccountLink();
    }
    else if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING)
    {
        frontendReadyFlag.store(false);
    }
}

// No model no stream (Bob Marley)
static void popupNoModelSelected()
{
	const std::string body = "No model is selected, you need to download a model from the "
                              "config page before using the filter.";
	showBabelStreamerPopup(PopupKind::NoModelSelected, "No Model Selected", body);
}

// Arm the event callback
void watchFrontendReady()
{
	obs_frontend_add_event_callback(frontendReadyEvent, nullptr);
}

// Disarn the event callback
void unwatchFrontendReady()
{
	obs_frontend_remove_event_callback(frontendReadyEvent, nullptr);
}

// Get the path for caching the device token which is needed to connect a session
static std::string deviceTokenCachePath()
{
	char *dir = obs_module_config_path(nullptr);
	if (!dir) return {};
	std::string path(dir);
	bfree(dir);
	if (!path.empty() && path.back() != '/' && path.back() != '\\')
		path += '/';
	path += "device_token";
	return path;
}

// Try and read the cached device token, empty string if unavailable
static std::string readCachedDeviceToken()
{
	const std::string path = deviceTokenCachePath();
	if (path.empty()) return {};
	std::ifstream in(path, std::ios::binary);
	if (!in) return {};
	std::string token((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	
    // Trim trailing whitespace/newline a text editor might add.
	while (!token.empty() && (token.back() == '\n' || token.back() == '\r' || token.back() == ' ' || token.back() == '\t'))
    {
        token.pop_back();
    }
	return token;
}

// Write the device token (if any) back to cache
static void writeCachedDeviceToken(const std::string &token)
{
	if (token.empty()) return;
	char *dir = obs_module_config_path(nullptr);
	if (dir)
    {
		os_mkdirs(dir);
		bfree(dir);
	}
	const std::string path = deviceTokenCachePath();
	if (path.empty()) return;
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (out) out << token;
}



/*
    Get the URL of the server api. This is different from the website download
    pages and is handled differently. Basic format is
 
            "<scheme>://<host>[:<port>]/api"
 
    which is all part of the parameter lists. The function has to handle several
    different ways of entering the url without freaking out. Assumption is that
    the server endpoint is at teh same place as the website endpoint, which may
    be changed in the future.
 */
static std::string deriveApiBase(WhisperFilter *f)
{
	std::string host;
	uint16_t port;
	bool tls;
	{
		std::lock_guard<std::mutex> lk(f->stateMtx);
		host = f->wsHost;
		port = f->wsPort;
		tls = f->wsUseTls;
	}
	if (host.empty()) 	return std::string();

	const char *scheme = tls ? "https" : "http";
	const uint16_t defPort = tls ? 443 : 80;
	std::string authority = host;
	if (port != 0 && port != defPort)
		authority += ":" + std::to_string(port);
	return std::string(scheme) + "://" + authority + "/api";
}


void filterDefaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, S_SPEAKER_NAME, "");
	obs_data_set_default_string(settings, S_TEXT_SOURCE_NAME, "");
	obs_data_set_default_bool(settings, S_SHOW_PARTIALS, false);
	obs_data_set_default_string(settings, S_WS_HOST, DEFAULT_WS_HOST);
	obs_data_set_default_int(settings, S_WS_PORT, DEFAULT_WS_PORT);
	obs_data_set_default_bool(settings, S_WS_USE_TLS, DEFAULT_WS_USE_TLS);
	obs_data_set_default_string(settings, S_DEVICE_TOKEN, "");
	obs_data_set_default_bool(settings, S_STREAM_GATE_ENABLED, DEFAULT_STREAM_GATE_ENABLED);
	obs_data_set_default_string(settings, S_SOURCE_LANG, DEFAULT_SOURCE_LANG);
	obs_data_set_default_string(settings, S_CUSTOM_VOCAB_FILE, "");
	obs_data_set_default_string(settings, S_GAME_PRESET, "");
	obs_data_set_default_int(settings, S_VAD_THRESHOLD, DEFAULT_VAD_THRESHOLD_PCT);
	obs_data_set_default_string(settings, S_GPU_DEVICE, DEFAULT_GPU_DEVICE);
	obs_data_set_default_int(settings, S_VAD_FRAME_MS, DEFAULT_VAD_FRAME_MS);
	obs_data_set_default_int(settings, S_VAD_SPEECH_MS, DEFAULT_VAD_SPEECH_MS);
	obs_data_set_default_int(settings, S_VAD_SILENCE_MS, DEFAULT_VAD_SILENCE_MS);
	obs_data_set_default_int(settings, S_MAX_UTTERANCE_MS, DEFAULT_MAX_UTTERANCE_MS);
	obs_data_set_default_int(settings, S_PARTIAL_EVERY_MS, DEFAULT_PARTIAL_EVERY_MS);
	obs_data_set_default_int(settings, S_PARTIAL_WINDOW_MS, DEFAULT_PARTIAL_WINDOW_MS);
}

/*
    Get the processing device options for the model download, this should
    match what we see in the properties page device selection
 */
static std::vector<ProcessingDeviceOption> processingDeviceOptions()
{
	std::vector<ProcessingDeviceOption> opts;
	for (const auto &dev : WhisperWrapper::listAvailableDevices())
		opts.push_back({dev.id, dev.label});
	opts.push_back({"cpu", "CPU (no GPU)"});
#if HAS_WHISPER_COREML
	if (isAppleSilicon()) opts.push_back({ANE_DEVICE_ID, "Apple Neural Engine (Core ML)"});
#endif
	return opts;
}

// Id of ANE device if available, otherwise empty
static std::string aneDeviceIdIfAvailable()
{
#if HAS_WHISPER_COREML
	if (isAppleSilicon()) return ANE_DEVICE_ID;
#endif
	return std::string();
}

/*
    Applies all the updates that the download dialogue defined:
    - model
    - processing device
    - source language
 */
static void applyStandardModelOutcome(WhisperFilter *filter, const StandardModelOutcome &out)
{
	if (out.modelPath.empty())  return;
	obs_data_t *settings = obs_source_get_settings(filter->context);
	obs_data_set_string(settings, S_MODEL_PATH, out.modelPath.c_str());
	if (!out.deviceId.empty()) obs_data_set_string(settings, S_GPU_DEVICE, out.deviceId.c_str());
	if (out.englishOnly)obs_data_set_string(settings, S_SOURCE_LANG, "en");
	obs_data_release(settings);
	obs_source_update(filter->context, nullptr);

    // Note: model is process wide so this applies to all filters
	if (out.englishOnly) applySourceLangToAll("en", filter);
	obs_source_update_properties(filter->context);
}


/*
    Handle the download model button. Usually this is to download the Whisper
    ggml model. However if there is already a model downloaded and selected,
    and there is a neural engine selected, then this switches function to
    download the ANE decoder
 */
static bool onDownloadModelClicked(obs_properties_t *, obs_property_t *, void *data)
{
	auto *filter = static_cast<WhisperFilter *>(data);
	if (!filter) return false;

#if HAS_WHISPER_COREML
    // Deal with ANE download form here
	if (isAppleSilicon() && filter->gpuDeviceId == ANE_DEVICE_ID && !filter->modelPath.empty())
    {
        // try and get the encoder
		CoremlEncoderResult res = CoremlEncoderResult::Failed;
		downloadCoremlEncoderFor(filter->modelPath, /*askFirst=*/false, [&res](CoremlEncoderResult r) { res = r; });
		if (res == CoremlEncoderResult::NotAvailable)
        {
			showBabelStreamerPopup(PopupKind::NoModelSelected, "Neural Engine",
					       "No Neural Engine model is published for this whisper model, so it "
					       "can only run on the CPU or the GPU. Choose a different processing "
					       "device, or switch to the standard model.");
		}
		if (res == CoremlEncoderResult::Installed)
        {
			// Remove the model and rebuild. Note that existing models in use
            // will be retained until the filter is finished
			WhisperModel::invalidate(filter->modelPath, effectiveDeviceId(filter));
			requestReloadAll();
		}
		return true;
	}
#endif

    // otherwise we just go to download a  standard model
	downloadStandardModelGuided(processingDeviceOptions(), filter->gpuDeviceId, aneDeviceIdIfAvailable(),
				    /*explainFirst=*/false,
				    [filter](const StandardModelOutcome &out) { applyStandardModelOutcome(filter, out); });
	return true;
}

/*
    Offer to download one of the standard models.
 */
void offerModelDownload(WhisperFilter *filter)
{
    // don't put this up until the scene load is done
	if (frontendReadyFlag.load())
    {
		runOnUiThreadLater([filter] {
			if (!filterStillRegistered(filter)) return; // destroyed while this was queued
			if (filter->modelPath.empty()) // may have sorted itself out while queued
            {
				downloadStandardModelGuided(processingDeviceOptions(), filter->gpuDeviceId,
							    aneDeviceIdIfAvailable(), /*explainFirst=*/true,
							    [filter](const StandardModelOutcome &out) {
								    applyStandardModelOutcome(filter, out);
							    });
			}
			// Do this afterwards do avoid multiple popups
			maybeOfferAccountLink();
		});
		return;
	}
    // throwaway thread to run the popup not selected warning
	std::thread(popupNoModelSelected).detach();
}


//forward declaration
static void aneAskTask(void *param);

/*
    Checks if the current neural engine decoder is the correct one the loaded language model.
    Reset mismatched models and to queue a request to load the model
 */
static void checkAnePairing(WhisperFilter *filter, obs_data_t *settings, const std::string &newModel,
                            const std::string &newGpuDevice)
{
#if HAS_WHISPER_COREML
	(void)settings;
	if (!isAppleSilicon()) return;

	/*
        filterUpdate() runs several times for one edit, so act on a pair that is
        new to this filter rather than on each apply so you want to avoid the
        popup running several times. This is done by keeping track of where you
        are in checking the model/device pair
     */
	const std::string pair = newModel + '\x1f' + newGpuDevice;
	if (pair == filter->anePairChecked) return;
	
    const bool wasPrimed = filter->anePairPrimed;
	filter->anePairChecked = pair;
	filter->anePairPrimed = true;

    // if we don't have an ANE or we don't have a new model, then abandon
	if (newGpuDevice != ANE_DEVICE_ID || newModel.empty()) return;

	std::error_code ec;
	if (std::filesystem::exists(coremlAdjacentPath(newModel), ec) || coremlEncoderCached(newModel))
		return; // there is already an encoder for this model


    // bail first time round, because it will be called once on startup when we are not ready
	if (!wasPrimed || !frontendReadyFlag.load())
    {
		blog(LOG_WARNING,
		     "[babelstreamer-filter] No Neural Engine encoder for '%s' - the encoder "
		     "will run on the CPU. Use \"Download Neural Engine Model\" in this filter's "
		     "settings to install one.",
		     newModel.c_str());
		return;
	}

	// Capture what to undo NOW: filterUpdate overwrites both of these a few
	// lines further on, long before the task runs.
	filter->aneRevertDevice = (newGpuDevice != filter->gpuDeviceId);
	filter->anePrevModel = filter->modelPath;
	filter->anePrevDevice = filter->gpuDeviceId;
	if (!filter->anePromptQueued)
    {
		filter->anePromptQueued = true;
		
        // aneAskTask is blocking, so run in different context
		runOnUiThreadLater([filter] { aneAskTask(filter); });
	}
#else
	(void)filter;
	(void)settings;
	(void)newModel;
	(void)newGpuDevice;
#endif
}


/*
    This is where we actually ask to download the neural engine. Note that we have
    to make sure we retain the previous state so that we can back down on error
    or if the user says no. Note also that this fuction will usually be called
    after being queued, so we have to check that the conditions for calling it
    are still valid.
 */
static void aneAskTask(void *param)
{
	auto *filter = static_cast<WhisperFilter *>(param);

	// Check that the filter wasn't disabled/deleted while we were queueing
	if (!filterStillRegistered(filter)) return;
	
    filter->anePromptQueued = false;

#if HAS_WHISPER_COREML
	/*
        Re-read rather than trusting what was captured: the streamer may have
        changed their mind again while this was queued.
     */
	const std::string model = filter->modelPath;
	const std::string device = filter->gpuDeviceId;
	if (device != ANE_DEVICE_ID || model.empty()) return;
	std::error_code ec;
	if (std::filesystem::exists(coremlAdjacentPath(model), ec) || coremlEncoderCached(model))
		return;

    // Try and download the model - mark failed and check later for success
	CoremlEncoderResult res = CoremlEncoderResult::Failed;
	downloadCoremlEncoderFor(model, /*askFirst=*/true, [&res](CoremlEncoderResult r) { res = r; });

    // If OK then we are done here
	if (res == CoremlEncoderResult::Installed)
    {
		WhisperModel::invalidate(model, "cpu"); // ANE's effective device
		requestReloadAll();
		return;
	}

	// We only reach here if the donwload failed, so we need to fallback
	const bool revertDevice = filter->aneRevertDevice;
	const std::string keptModel = revertDevice ? model : filter->anePrevModel;
	const std::string keptDevice = revertDevice ? filter->anePrevDevice : device;
	
    // Mark that we've checked this previous model to avoid multiple popups being queued
	filter->anePairChecked = keptModel + '\x1f' + keptDevice;

    // Update the settings with the correct device or model
	obs_data_t *settings = obs_source_get_settings(filter->context);
	obs_data_set_string(settings, revertDevice ? S_GPU_DEVICE : S_MODEL_PATH,
			    (revertDevice ? keptDevice : keptModel).c_str());
	obs_data_release(settings);
	obs_source_update(filter->context, nullptr);

	blog(LOG_INFO, "[babelstreamer-filter] Neural Engine declined or unavailable - %s",
	     revertDevice ? "staying on the previous processing device" : "keeping the previous model");

	if (res == CoremlEncoderResult::NotAvailable) {
		showBabelStreamerPopup(PopupKind::NoModelSelected, "Neural Engine",
				       revertDevice ? "No Neural Engine model is published for the model you have "
						      "selected, so it can only run on the CPU or the GPU. The "
						      "processing device has been left unchanged."
						    : "No Neural Engine model is published for that whisper model. "
						      "Your previous model has been kept - pick a different processing "
						      "device to use the new one.");
	}
	// Last, so the properties rebuild can't pull the rug from under a dialog
	// that is still open.
	obs_source_update_properties(filter->context);
#endif
}

#if HAS_WHISPER_COREML
/*
    Make sure the button text matches the current context - either download
    the translation model or download the ANE decoder
 */
static bool onGpuDeviceChanged(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	obs_property_t *btn = obs_properties_get(props, "download_standard_model");
	if (!btn) return false; // no Qt UI (button not added) - nothing to relabel
	
    const char *dev = obs_data_get_string(settings, S_GPU_DEVICE);
	const bool neural = dev && std::string(dev) == ANE_DEVICE_ID;
	obs_property_set_description(btn, neural ? "Download Neural Engine Model" : "Download Standard Model");
	return true;
}
#endif

/* Link to server for help */
static bool onHelpClicked(obs_properties_t *, obs_property_t *, void *)
{
	QDesktopServices::openUrl(QUrl(QStringLiteral("https://babelstreamer.com/manual.html")));
	return false; // nothing changed, nothing to redraw
}

/*
    connect account button runs the devie pairing function. May block
    as this is a user action
 */
static bool onConnectAccountClicked(obs_properties_t *, obs_property_t *, void *data)
{
	auto *filter = static_cast<WhisperFilter *>(data);
	if (!filter) return false;

	bool changed = false;
	connectAccountViaDeviceCode(deriveApiBase(filter),
				    [filter, &changed](const std::string &token, const std::string & /*label*/) {
					    if (token.empty())
						    return;
					    writeCachedDeviceToken(token);
					    obs_data_t *settings = obs_source_get_settings(filter->context);
					    obs_data_set_string(settings, S_DEVICE_TOKEN, token.c_str());
					    obs_data_release(settings);
					    obs_source_update(filter->context, nullptr);
					    changed = true;
				    });
	return changed; // redraw the properties dialog so the token field fills in
}

/*
    Offer to link the account. Only do once per session, latched on accountOfferMade
    The guts of the function runs on a sperate thread because it will usually be
    called on the load completion trigger which may not block.
 */
static std::atomic<bool> accountOfferMade{false};
void maybeOfferAccountLink()
{
	if (!frontendReadyFlag.load()) return;
	if (accountOfferMade.load()) return;

	runOnUiThreadLater([] {
		// A token is system wide but we need to link it to a specific filter
		WhisperFilter *filter = firstRegisteredFilter();
		if (!filter) return; // nothing to connect yet - the next filterCreate asks again
		if (!readCachedDeviceToken().empty()) return; // already connected
		
        {
			std::lock_guard<std::mutex> lock(filter->stateMtx);
			if (!filter->deviceToken.empty()) return;
		}
        
		// We're not bailing out, so we can latch the offer now
		if (accountOfferMade.exchange(true)) return;

        // Before we do it, ask politely
		if (!askToConnectAccount())
        {
			blog(LOG_INFO, "[babelstreamer-filter] No account connected - the streamer chose to "
				       "link later with the \"Connect account\" button");
			return;
		}
        
        // and now we do the actual connection (which might fail BTW)
		connectAccountViaDeviceCode(deriveApiBase(filter),
					    [filter](const std::string &token, const std::string & /*label*/) {
						    if (token.empty())
							    return;
						    writeCachedDeviceToken(token);
						    obs_data_t *settings = obs_source_get_settings(filter->context);
						    obs_data_set_string(settings, S_DEVICE_TOKEN, token.c_str());
						    obs_data_release(settings);
						    obs_source_update(filter->context, nullptr);
						    obs_source_update_properties(filter->context);
					    });
	});
}

// OBS calls this to set up the filter properties
obs_properties_t *filterProperties(void *data)
{
	auto *filter = static_cast<WhisperFilter *>(data);
	obs_properties_t *props = obs_properties_create();
	refreshGameVocabulary();  //  Update the the current game list asynchronously

    // Only the master (first, by OBS's own source/filter order) filter can
    // edit the plugin-wide device/model settings - see FilterOrigin.hpp's
    // isMasterFilterSource(). Every other filter's copies of those fields
    // are made read-only further down.
    const bool isMaster = filter && isMasterFilterSource(filter->context);

	obs_properties_add_button(props, "get_help", "Get Help on these settings", onHelpClicked);
	
    // Do the read-only status fields
    bool hasAccount = false;
	if (filter)
    {
		std::lock_guard<std::mutex> lock(filter->stateMtx);
		hasAccount = !filter->deviceToken.empty();
	}

	if (filter)
    {
		obs_data_t *liveSettings = obs_source_get_settings(filter->context);
		if (hasAccount)
        {
			std::string sessionKey;
			{
				// Written by the sender thread on every session frame.
				std::lock_guard<std::mutex> lk(filter->stateMtx);
				sessionKey = ServerLink::getInstance().serverLinkSessionKey();
			}
			obs_data_set_string(liveSettings, S_SESSION_KEY,
					    sessionKey.empty() ? "(not connected)" : sessionKey.c_str());
		}
		obs_data_set_string(liveSettings, S_PLUGIN_VERSION, PLUGIN_VERSION);
		obs_data_release(liveSettings);
	}

    // Session key is only relevant if you actually have a linked account
	if (hasAccount)
    {
		obs_properties_add_text(props, S_SESSION_KEY, "Session key", OBS_TEXT_DEFAULT);
		obs_property_set_enabled(obs_properties_get(props, S_SESSION_KEY), false);
	}

	obs_properties_add_text(props, S_PLUGIN_VERSION, "Version", OBS_TEXT_DEFAULT);
	obs_property_set_enabled(obs_properties_get(props, S_PLUGIN_VERSION), false);

    // Speaker name is optional per filter
	obs_properties_add_text(props, S_SPEAKER_NAME, "Speaker name (optional)", OBS_TEXT_DEFAULT);
	obs_property_set_long_description(obs_properties_get(props, S_SPEAKER_NAME),
					  "Shown to viewers in front of this source's captions, e.g. {Alice}, "
					  "but only while more than one filter is active - with a single active "
					  "source there's nothing to distinguish it from, so the name is left off. "
					  "Use one filter per speaker (separate mics, or Discord participants on "
					  "separate tracks) and give each its own name. Leave blank for a single "
					  "unattributed source.");
    
    // Choose a text overlay to send captions to
	obs_property_t *srcList = obs_properties_add_list(props, S_TEXT_SOURCE_NAME, "Caption Text Output",
							  OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	obs_property_list_add_string(srcList, "(none - capture only)", "");

	// Enumerate all sources so the user can pick whichever Text source they
	// created, regardless of which FreeType2/GDI+ variant OBS installed.
	struct EnumCtx
    {
		obs_property_t *list;
	};
	EnumCtx ctx{srcList};

	obs_enum_sources(
		[](void *param, obs_source_t *source) -> bool {
			const char *id = obs_source_get_id(source);
			// Accept any source whose type ID contains "text"
			if (id && strstr(id, "text"))
            {
				const char *name = obs_source_get_name(source);
				obs_property_list_add_string(static_cast<EnumCtx *>(param)->list, name, name);
			}
			return true;
		},
		&ctx);

	// Model - this is for when you enter a model name manually
	obs_properties_add_path(props, S_MODEL_PATH, "Model (.bin)", OBS_PATH_FILE, "*.bin", nullptr);
	obs_property_set_long_description(obs_properties_get(props, S_MODEL_PATH),
					  "Shared by every BabelStreamer filter in this OBS - changing it here "
					  "changes it for all of them. One loaded model is shared across all "
					  "sources, so a filter pointed at a different file would load a second "
					  "multi-gigabyte copy and can exhaust the GPU.");

	// Processing devices: GPUs, Metal, ANE and CPU as fallback
	obs_property_t *gpuList = obs_properties_add_list(props, S_GPU_DEVICE, "Processing device", OBS_COMBO_TYPE_LIST,
							  OBS_COMBO_FORMAT_STRING);
	for (const auto &dev : WhisperWrapper::listAvailableDevices())
    {
        obs_property_list_add_string(gpuList, dev.label.c_str(), dev.id.c_str());
    }
	obs_property_list_add_string(gpuList, "CPU (no GPU)", "cpu");
	obs_property_set_long_description(gpuList,
					  "Shared by every BabelStreamer filter in this OBS - changing it here "
					  "changes it for all of them, and reloads each onto the new device. It "
					  "has to be shared: the Core ML encoder is selected by a file next to the "
					  "model, so filters using the same model cannot run on different devices.");
#if HAS_WHISPER_COREML
	// ANE requires a one time download to match the loaded model
	if (isAppleSilicon())
    {
		obs_property_list_add_string(gpuList, "Apple Neural Engine (Core ML)", ANE_DEVICE_ID);
		obs_property_set_modified_callback(gpuList, onGpuDeviceChanged);
	}
#endif

	// Download model or ANE decoder, depending on context
	obs_properties_add_button(props, "download_standard_model", "Download Standard Model", onDownloadModelClicked);
#if HAS_WHISPER_COREML
	// The modified callback only fires on later changes, so set the button's
	// initial label to match the already-saved device (the button exists now).
	if (isAppleSilicon()) {
		if (obs_data_t *s = obs_source_get_settings(filter->context)) {
			const char *dev = obs_data_get_string(s, S_GPU_DEVICE);
			if (dev && std::string(dev) == ANE_DEVICE_ID)
				if (obs_property_t *btn = obs_properties_get(props, "download_standard_model"))
					obs_property_set_description(btn, "Download Neural Engine Model");
			obs_data_release(s);
		}
	}
#endif


    // Followers can't edit the model/device - grey them out and show the
    // real shared value (their own copies may be stale, since a follower's
    // filterUpdate() only runs when its own panel is applied, not whenever
    // the master publishes a change).
    if (filter && !isMaster)
    {
        obs_data_t *liveSettings = obs_source_get_settings(filter->context);
        obs_data_set_string(liveSettings, S_MODEL_PATH, readStoredModelPath().c_str());
        obs_data_set_string(liveSettings, S_GPU_DEVICE, readStoredProcessingDevice().c_str());
        obs_data_release(liveSettings);

        obs_property_set_enabled(obs_properties_get(props, S_MODEL_PATH), false);
        obs_property_set_enabled(obs_properties_get(props, S_GPU_DEVICE), false);
        if (obs_property_t *downloadBtn = obs_properties_get(props, "download_standard_model"))
            obs_property_set_enabled(downloadBtn, false);

        const char *followerHint = "Shared by every BabelStreamer filter in this OBS. Controlled by the "
                                    "master filter - the first one in your Sources list - open its "
                                    "properties to change it.";
        obs_property_set_long_description(obs_properties_get(props, S_MODEL_PATH), followerHint);
        obs_property_set_long_description(obs_properties_get(props, S_GPU_DEVICE), followerHint);
    }

	// Connect account button
	obs_properties_add_button(props, "connect_account", "Connect account", onConnectAccountClicked);

	// Account token - password masked with reveal button
	obs_properties_add_text(props, S_DEVICE_TOKEN, "Token (from account)", OBS_TEXT_PASSWORD);

	// Stream gate - only connect to server when streaming
	obs_properties_add_bool(props, S_STREAM_GATE_ENABLED,
				"Only connect while live streaming "
				"(uncheck to always connect - for testing)");

	
    // Choose an input language. Also dependent on model, English only models dont
    // have much to choose :-)
	obs_property_t *langList = obs_properties_add_list(props, S_SOURCE_LANG, "Speech Language", OBS_COMBO_TYPE_LIST,
							   OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(langList, "Auto-detect", "auto");
	// These four are the ones that the translation server is optimised for
	obs_property_list_add_string(langList, "English", "en");
	obs_property_list_add_string(langList, "Español (Spanish)", "es");
	obs_property_list_add_string(langList, "Deutsch (German)", "de");
	obs_property_list_add_string(langList, "Français (French)", "fr");
	
    // Every other language is possible, but not optimised translation (see comment below)
	obs_property_list_add_string(langList, "Afrikaans", "af");
	obs_property_list_add_string(langList, "Albanian", "sq");
	obs_property_list_add_string(langList, "Amharic", "am");
	obs_property_list_add_string(langList, "Arabic", "ar");
	obs_property_list_add_string(langList, "Armenian", "hy");
	obs_property_list_add_string(langList, "Assamese", "as");
	obs_property_list_add_string(langList, "Azerbaijani", "az");
	obs_property_list_add_string(langList, "Bashkir", "ba");
	obs_property_list_add_string(langList, "Basque", "eu");
	obs_property_list_add_string(langList, "Belarusian", "be");
	obs_property_list_add_string(langList, "Bengali", "bn");
	obs_property_list_add_string(langList, "Bosnian", "bs");
	obs_property_list_add_string(langList, "Breton", "br");
	obs_property_list_add_string(langList, "Bulgarian", "bg");
	obs_property_list_add_string(langList, "Cantonese", "yue");
	obs_property_list_add_string(langList, "Catalan", "ca");
	obs_property_list_add_string(langList, "Chinese", "zh");
	obs_property_list_add_string(langList, "Croatian", "hr");
	obs_property_list_add_string(langList, "Czech", "cs");
	obs_property_list_add_string(langList, "Danish", "da");
	obs_property_list_add_string(langList, "Dutch", "nl");
	obs_property_list_add_string(langList, "Estonian", "et");
	obs_property_list_add_string(langList, "Faroese", "fo");
	obs_property_list_add_string(langList, "Finnish", "fi");
	obs_property_list_add_string(langList, "Galician", "gl");
	obs_property_list_add_string(langList, "Georgian", "ka");
	obs_property_list_add_string(langList, "Greek", "el");
	obs_property_list_add_string(langList, "Gujarati", "gu");
	obs_property_list_add_string(langList, "Haitian Creole", "ht");
	obs_property_list_add_string(langList, "Hausa", "ha");
	obs_property_list_add_string(langList, "Hawaiian", "haw");
	obs_property_list_add_string(langList, "Hebrew", "he");
	obs_property_list_add_string(langList, "Hindi", "hi");
	obs_property_list_add_string(langList, "Hungarian", "hu");
	obs_property_list_add_string(langList, "Icelandic", "is");
	obs_property_list_add_string(langList, "Indonesian", "id");
	obs_property_list_add_string(langList, "Italian", "it");
	obs_property_list_add_string(langList, "Japanese", "ja");
	obs_property_list_add_string(langList, "Javanese", "jw");
	obs_property_list_add_string(langList, "Kannada", "kn");
	obs_property_list_add_string(langList, "Kazakh", "kk");
	obs_property_list_add_string(langList, "Khmer", "km");
	obs_property_list_add_string(langList, "Korean", "ko");
	obs_property_list_add_string(langList, "Lao", "lo");
	obs_property_list_add_string(langList, "Latin", "la");
	obs_property_list_add_string(langList, "Latvian", "lv");
	obs_property_list_add_string(langList, "Lingala", "ln");
	obs_property_list_add_string(langList, "Lithuanian", "lt");
	obs_property_list_add_string(langList, "Luxembourgish", "lb");
	obs_property_list_add_string(langList, "Macedonian", "mk");
	obs_property_list_add_string(langList, "Malagasy", "mg");
	obs_property_list_add_string(langList, "Malay", "ms");
	obs_property_list_add_string(langList, "Malayalam", "ml");
	obs_property_list_add_string(langList, "Maltese", "mt");
	obs_property_list_add_string(langList, "Maori", "mi");
	obs_property_list_add_string(langList, "Marathi", "mr");
	obs_property_list_add_string(langList, "Mongolian", "mn");
	obs_property_list_add_string(langList, "Myanmar", "my");
	obs_property_list_add_string(langList, "Nepali", "ne");
	obs_property_list_add_string(langList, "Norwegian", "no");
	obs_property_list_add_string(langList, "Nynorsk", "nn");
	obs_property_list_add_string(langList, "Occitan", "oc");
	obs_property_list_add_string(langList, "Pashto", "ps");
	obs_property_list_add_string(langList, "Persian", "fa");
	obs_property_list_add_string(langList, "Polish", "pl");
	obs_property_list_add_string(langList, "Portuguese", "pt");
	obs_property_list_add_string(langList, "Punjabi", "pa");
	obs_property_list_add_string(langList, "Romanian", "ro");
	obs_property_list_add_string(langList, "Russian", "ru");
	obs_property_list_add_string(langList, "Sanskrit", "sa");
	obs_property_list_add_string(langList, "Serbian", "sr");
	obs_property_list_add_string(langList, "Shona", "sn");
	obs_property_list_add_string(langList, "Sindhi", "sd");
	obs_property_list_add_string(langList, "Sinhala", "si");
	obs_property_list_add_string(langList, "Slovak", "sk");
	obs_property_list_add_string(langList, "Slovenian", "sl");
	obs_property_list_add_string(langList, "Somali", "so");
	obs_property_list_add_string(langList, "Sundanese", "su");
	obs_property_list_add_string(langList, "Swahili", "sw");
	obs_property_list_add_string(langList, "Swedish", "sv");
	obs_property_list_add_string(langList, "Tagalog", "tl");
	obs_property_list_add_string(langList, "Tajik", "tg");
	obs_property_list_add_string(langList, "Tamil", "ta");
	obs_property_list_add_string(langList, "Tatar", "tt");
	obs_property_list_add_string(langList, "Telugu", "te");
	obs_property_list_add_string(langList, "Thai", "th");
	obs_property_list_add_string(langList, "Tibetan", "bo");
	obs_property_list_add_string(langList, "Turkish", "tr");
	obs_property_list_add_string(langList, "Turkmen", "tk");
	obs_property_list_add_string(langList, "Ukrainian", "uk");
	obs_property_list_add_string(langList, "Urdu", "ur");
	obs_property_list_add_string(langList, "Uzbek", "uz");
	obs_property_list_add_string(langList, "Vietnamese", "vi");
	obs_property_list_add_string(langList, "Welsh", "cy");
	obs_property_list_add_string(langList, "Yiddish", "yi");
	obs_property_list_add_string(langList, "Yoruba", "yo");
	obs_property_set_long_description(langList, "Whisper transcribes any of these locally, with no account needed. "
						    "Only English, Spanish, German and French are also translated for "
						    "remote viewers once an account is connected - every other "
						    "language gives you local captions on your own screen, not a "
						    "translated feed for your audience.");

	// Game list for optimised vocab, using downloaded or cached lists
	obs_property_t *gameList = obs_properties_add_list(props, S_GAME_PRESET, "Game (auto-inject vocabulary)",
							   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(gameList, "(None)", "");
	for (const std::string &name : cachedGameNames())
    {
        obs_property_list_add_string(gameList, name.c_str(), name.c_str());
    }

	// Custom vocab - extra injection of user custom vocab filr
	obs_properties_add_path(props, S_CUSTOM_VOCAB_FILE, "Custom Vocab File", OBS_PATH_FILE, "*.txt", nullptr);

	// Speech recognition threshhold
	obs_properties_add_int(props, S_VAD_THRESHOLD, "Speech Threshold (%)", 5, 95, 5);

	// Whisper speech detection params: minimum, slience length, cap on long speech
	obs_properties_add_int(props, S_VAD_SPEECH_MS, "Min Speech (ms)", 30, 500, 10);
	obs_properties_add_int(props, S_VAD_SILENCE_MS, "End Silence (ms)", 50, 1000, 10);
	obs_properties_add_int(props, S_MAX_UTTERANCE_MS, "Max Utterance (ms, 0 = unlimited)", 0, 120000, 1000);

	// Some advanced/debugging features
	obs_properties_add_text(props, S_WS_HOST, "Server address", OBS_TEXT_DEFAULT);
	obs_properties_add_int(props, S_WS_PORT, "Server Port", 1, 65535, 1);

    // Auto on if server adress includes wss://
	obs_properties_add_bool(props, S_WS_USE_TLS, "Use secure connection (wss://)");

    // Only the master filter has access to these properties
    if (filter && !isMaster)
    {
        obs_property_set_enabled(obs_properties_get(props, "connect_account"), false);
        obs_property_set_enabled(obs_properties_get(props, S_DEVICE_TOKEN), false);
        obs_property_set_enabled(obs_properties_get(props, S_WS_HOST), false);
        obs_property_set_enabled(obs_properties_get(props, S_WS_PORT), false);
        obs_property_set_enabled(obs_properties_get(props, S_WS_USE_TLS), false);

        const char *connectionHint = "Shared by every BabelStreamer filter in this OBS. Controlled by the "
                                      "master filter - the first one in your Sources list - open its "
                                      "properties to change it.";
        obs_property_set_long_description(obs_properties_get(props, S_DEVICE_TOKEN), connectionHint);
        obs_property_set_long_description(obs_properties_get(props, S_WS_HOST), connectionHint);
    }

	return props;
}


/*
 Convert the server address fields to a normalised host/port pair. Mainly useful
    for testing.
 
    Accepts a bare host ("myserver.com"), host:port, an IPv6 literal in
    brackets ("[::1]:8090"), or the same prefixed with any scheme
   ("ws://", "wss://", "http://", "https://") and/or followed by a
   "/path?query" - always resolves down to a plain host + optional port
    override.
 */
static std::pair<std::string, uint16_t> parseWsHostInput(const std::string &rawHost, uint16_t fallbackPort,
							 bool &detectedTls)
{
	std::string s = rawHost;

    // teeny helper
	auto notSpace = [](unsigned char c)
    {
		return !std::isspace(c);
	};
    
	s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
	s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());

	if (s.empty()) return {s, fallbackPort};

	// Strip a leading "scheme://", whatever the scheme but note if it was secure
	size_t schemeEnd = s.find("://");
	if (schemeEnd != std::string::npos)
    {
		std::string scheme = s.substr(0, schemeEnd);
		std::transform(scheme.begin(), scheme.end(), scheme.begin(),
			       [](unsigned char c) { return (char)std::tolower(c); });
		if (scheme == "wss" || scheme == "https")
			detectedTls = true;
		else if (scheme == "ws" || scheme == "http")
			detectedTls = false;
		s = s.substr(schemeEnd + 3);
	}

	// Drop any trailing path/query - the plugin builds its own path.
	size_t slash = s.find('/');
	if (slash != std::string::npos) s = s.substr(0, slash);

	if (s.empty())return {s, fallbackPort};

	uint16_t port = fallbackPort;
	std::string host = s;

    // Another teeny helper
	auto isAllDigits = [](const std::string &str) {
		return !str.empty() &&
		       std::all_of(str.begin(), str.end(), [](unsigned char c) { return std::isdigit(c); });
	};

	if (s.front() == '[')
    {
		// IPv6 literal, e.g. [::1]:8090
		size_t close = s.find(']');
		if (close != std::string::npos)
        {
			host = s.substr(1, close - 1);
			if (close + 1 < s.size() && s[close + 1] == ':')
            {
				std::string portStr = s.substr(close + 2);
				if (isAllDigits(portStr))
                {
					long p = std::strtol(portStr.c_str(), nullptr, 10);
					if (p >= 1 && p <= 65535) port = (uint16_t)p;
				}
			}
		}
	}
    else
    {
		size_t colon = s.rfind(':');
		if (colon != std::string::npos)
        {
			std::string portStr = s.substr(colon + 1);
			if (isAllDigits(portStr))
            {
				long p = std::strtol(portStr.c_str(), nullptr, 10);
				if (p >= 1 && p <= 65535)
                {
					host = s.substr(0, colon);
					port = (uint16_t)p;
				}
			}
		}
	}

	return {host, port};
}

/*
    Reads the user's custom vocab file into a single string for whisper's initial_prompt.
    
    Format ois
    - one term per line
    - blank lines and lines prefixed with '#' are ignored
    
    Note that this is added last to the prompr so this is the list that gets truncated
    if the token budget is exceeded.
 */
static std::string loadCustomVocabFile(const std::string &path)
{
	if (path.empty())return {};

	std::ifstream file(path);
	if (!file.is_open())
    {
		blog(LOG_WARNING, "[babelstreamer-filter] Could not open custom vocabulary file: %s", path.c_str());
		return {};
	}

	std::string out;
	std::string line;
	while (std::getline(file, line))
    {
		// Trim whitespace - also strips a trailing '\r' from CRLF-saved files.
		size_t a = line.find_first_not_of(" \t\r\n");
		if (a == std::string::npos)
			continue;
		size_t b = line.find_last_not_of(" \t\r\n");
		line = line.substr(a, b - a + 1);

		if (line.empty() || line.front() == '#')
			continue;

		if (!out.empty())
			out += ", ";
		out += line;
	}
	return out;
}

/*
    This gets called from lots of places when parameters changed
    and the filter needs to be updated to reflect the parameters
 */
void filterUpdate(void *data, obs_data_t *settings)
{
	auto *filter = static_cast<WhisperFilter *>(data);

	// These variables are read on every audio cycle, so protect them
	{
		std::lock_guard<std::mutex> lk(filter->stateMtx);
		filter->speakerName = obs_data_get_string(settings, S_SPEAKER_NAME);
		filter->textSourceName = obs_data_get_string(settings, S_TEXT_SOURCE_NAME);
		filter->showPartials = obs_data_get_bool(settings, S_SHOW_PARTIALS);
	}

	std::string newModel = obs_data_get_string(settings, S_MODEL_PATH);
	std::string rawHost = obs_data_get_string(settings, S_WS_HOST);
	uint16_t rawPort = (uint16_t)obs_data_get_int(settings, S_WS_PORT);
	bool rawUseTls = obs_data_get_bool(settings, S_WS_USE_TLS);
	bool newUseTls = rawUseTls;
	auto [newHost, newPort] = parseWsHostInput(rawHost, rawPort, newUseTls);
  
    // If there's no token in the UI then fall back to the cached token, if any
    std::string newDeviceToken = obs_data_get_string(settings, S_DEVICE_TOKEN);
	if (newDeviceToken.empty())
    {
		std::string cached = readCachedDeviceToken();
		if (!cached.empty())
        {
			newDeviceToken = cached;
			obs_data_set_string(settings, S_DEVICE_TOKEN, cached.c_str());
		}
	}
    else if (newDeviceToken != readCachedDeviceToken())
    {
		writeCachedDeviceToken(newDeviceToken);
	}
    
	bool newStreamGateEnabled = obs_data_get_bool(settings, S_STREAM_GATE_ENABLED);
	std::string newLang = obs_data_get_string(settings, S_SOURCE_LANG);
	std::string newCustomVocabPath = obs_data_get_string(settings, S_CUSTOM_VOCAB_FILE);
	std::string newCustomVocab = loadCustomVocabFile(newCustomVocabPath);
	std::string newGamePreset = obs_data_get_string(settings, S_GAME_PRESET);
	
    
    // Whisper worker callback reads every cyle so needs protecting
    {
		std::lock_guard<std::mutex> lk(filter->stateMtx);
		filter->gamePreset = newGamePreset;
	}

    /*
        We build an initial prompt here and order is important. Whisper will
        truncate if the max token count is exceeded, so we put the most
        important terms first
     */
    
    std::string newGameTerms = cachedGameTerms(newGamePreset);
	if (!newGameTerms.empty())
    {
		newCustomVocab = newCustomVocab.empty() ? newGameTerms : newCustomVocab + ", " + newGameTerms;
	}
    
    
	int newVadThresholdPct = (int)obs_data_get_int(settings, S_VAD_THRESHOLD);
	
    /*
        Processing device and model are shared across every filter, but only
        the MASTER filter's panel is allowed to change them (see
        FilterOrigin.hpp's isMasterFilterSource() - the first filter in OBS's
        own source/filter order). The master reconciles its panel against the
        shared value exactly as before (reconcileShared, below). A follower's
        panel is read-only (see filterProperties()), so there is nothing to
        reconcile for it - it just mirrors whatever the master has published.
     */
    const bool isMaster = isMasterFilterSource(filter->context);
    std::string newGpuDevice = obs_data_get_string(settings, S_GPU_DEVICE);
	if (isMaster)
	{
		const std::string stored = readStoredProcessingDevice();
		switch (reconcileShared(!stored.empty(), filter->deviceChoiceSeen,
                                obs_data_has_user_value(settings, S_GPU_DEVICE), newGpuDevice == stored))
        {
		case SharedAction::Adopt:
			newGpuDevice = stored;
			obs_data_set_string(settings, S_GPU_DEVICE, stored.c_str());
			break;
                
		case SharedAction::Donate:
			writeStoredProcessingDevice(newGpuDevice);
			blog(LOG_INFO,
			     "[babelstreamer-filter] Adopted '%s' as the plugin-wide processing device "
			     "from an existing filter's saved setting",
			     newGpuDevice.c_str());
			break;
                
		case SharedAction::ComputeDefault:
			newGpuDevice = resolveProcessingDevice();
			obs_data_set_string(settings, S_GPU_DEVICE, newGpuDevice.c_str());
			break;
                
		case SharedAction::Publish:
			applyProcessingDeviceToAll(newGpuDevice, filter);
			break;
                
		case SharedAction::None:
			break;
		}
		filter->deviceChoiceSeen = true;

		// Same thing with the model choice (don't worry about ANE as this always follows model)
		const std::string storedModel = readStoredModelPath();
		switch (reconcileShared(!storedModel.empty(), filter->modelChoiceSeen,
                                 obs_data_has_user_value(settings, S_MODEL_PATH), newModel == storedModel))
        {
		case SharedAction::Adopt:
			newModel = storedModel;
			obs_data_set_string(settings, S_MODEL_PATH, storedModel.c_str());
			break;
                
		case SharedAction::Donate:
			// A filter that has no model has nothing to donate. writeStoredSetting
			// already discards the empty write; this keeps the log honest too,
			// instead of announcing that '' is now the plugin-wide model.
			if (!newModel.empty())
        {
				writeStoredModelPath(newModel);
				blog(LOG_INFO,
				     "[babelstreamer-filter] Took '%s' from this filter's saved settings "
				     "as the plugin-wide model",
				     newModel.c_str());
			}
			break;
                
		case SharedAction::Publish:
			applyModelToAll(newModel, filter);
			break;
                
		case SharedAction::ComputeDefault:
		case SharedAction::None:
			break;
		}
		filter->modelChoiceSeen = true;
	}
	else
	{
		// Follower: no panel value of ours is ever meaningful, so just mirror
		// the shared value and keep our own panel in step (it's disabled, but
		// should still show the truth if the user looks at it).
		newGpuDevice = readStoredProcessingDevice();
		obs_data_set_string(settings, S_GPU_DEVICE, newGpuDevice.c_str());
		newModel = readStoredModelPath();
		obs_data_set_string(settings, S_MODEL_PATH, newModel.c_str());
	}
	int newFrameMs = (int)obs_data_get_int(settings, S_VAD_FRAME_MS);
	int newSpeechMs = (int)obs_data_get_int(settings, S_VAD_SPEECH_MS);
	int newSilenceMs = (int)obs_data_get_int(settings, S_VAD_SILENCE_MS);
	int newMaxUtteranceMs = (int)obs_data_get_int(settings, S_MAX_UTTERANCE_MS);
	int newPartialEvery = (int)obs_data_get_int(settings, S_PARTIAL_EVERY_MS);
	int newPartialWindow = (int)obs_data_get_int(settings, S_PARTIAL_WINDOW_MS);


    // This checks the processor/model combo and if necessary queues a ANE update
    // prompt. Only relevant for the master: a follower never chose the pairing,
    // it just mirrors whatever the master already resolved.
	if (isMaster)
		checkAnePairing(filter, settings, newModel, newGpuDevice);

    
    // Check if we need a reload
	bool reloadNeeded = (newModel != filter->modelPath || newLang != filter->sourceLang || newGpuDevice != filter->gpuDeviceId);

    // All other data can be changed on the fly without a reload
	bool liveChanged = (newCustomVocab != filter->customVocab || newFrameMs != filter->vadFrameMs ||
			    newSpeechMs != filter->vadSpeechMs || newSilenceMs != filter->vadSilenceMs ||
			    newMaxUtteranceMs != filter->maxUtteranceMs || newPartialEvery != filter->partialEveryMs ||
			    newPartialWindow != filter->partialWindowMs || newVadThresholdPct != filter->vadThresholdPct);

	// Put the resolved networking settings back into the settings block
	if (newHost != rawHost) obs_data_set_string(settings, S_WS_HOST, newHost.c_str());
	if (newPort != rawPort) obs_data_set_int(settings, S_WS_PORT, newPort);
	if (newUseTls != rawUseTls) obs_data_set_bool(settings, S_WS_USE_TLS, newUseTls);

	// Store updates to filter. Networking settings are used by network thread so need mutex
    filter->modelPath = newModel;
	filter->sourceLang = newLang.empty() ? DEFAULT_SOURCE_LANG : newLang;
	filter->customVocab = newCustomVocab;
	filter->gpuDeviceId = newGpuDevice.empty() ? DEFAULT_GPU_DEVICE : newGpuDevice;
	filter->vadFrameMs = newFrameMs;
	filter->vadSpeechMs = newSpeechMs;
	filter->vadSilenceMs = newSilenceMs;
	filter->maxUtteranceMs = newMaxUtteranceMs;
	filter->partialEveryMs = newPartialEvery;
	filter->partialWindowMs = newPartialWindow;
	filter->vadThresholdPct = newVadThresholdPct;
	filter->streamGateEnabled.store(newStreamGateEnabled);
    {
        std::lock_guard<std::mutex> lk(filter->stateMtx);
        filter->wsHost = newHost;
        filter->wsPort = newPort;
        filter->wsUseTls = newUseTls;
        filter->deviceToken = newDeviceToken;
    }

	// Tell the server handler about the new settings - it decides what to do with them
	ServerLink::getInstance().serverLinkUpdate(filter->linkId, makeLinkParams(filter));
	ServerLink::getInstance().serverLinkApplyStreamGate();

	// Reload the model only when something actually requires it; otherwise
	// hot-swap the live settings on the running instance.
	if (reloadNeeded)
    {
		blog(LOG_INFO, "[babelstreamer-filter] Reloading whisper (model, language or GPU device changed)");
		requestReload(filter);
	}
    else if (!filter->whisper && !filter->modelPath.empty())
    {
		// A model is configured but isn't loaded, so load it
		blog(LOG_INFO, "[babelstreamer-filter] Whisper isn't loaded - retrying model load");
		requestReload(filter);
	}
    else if (liveChanged)
    {
		std::lock_guard<std::mutex> lk(filter->whisperMtx);
		if (filter->whisper)
        {
			filter->whisper->updateLiveConfig(makeLiveConfig(filter));
			blog(LOG_INFO, "[babelstreamer-filter] Applied VAD/vocabulary settings without a reload");
		}
	}

	blog(LOG_INFO,
	     "[babelstreamer-filter] Settings: ws://%s:%u  lang=%s  gpu_device=%s  "
	     "frame=%dms  speech=%dms  silence=%dms  max_utterance=%dms  partial=%dms/%dms",
	     filter->wsHost.c_str(), filter->wsPort, filter->sourceLang.c_str(), filter->gpuDeviceId.c_str(), filter->vadFrameMs, filter->vadSpeechMs,
	     filter->vadSilenceMs, filter->maxUtteranceMs, filter->partialEveryMs, filter->partialWindowMs);
}
