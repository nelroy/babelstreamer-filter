// SPDX-FileCopyrightText: 2026 Kamayu B.V. <https://www.kamayu.nl>
// SPDX-License-Identifier: GPL-2.0-or-later

//=======================================================================
//
//  SharedSettings.cpp
//
//  Created: 26 sep 2026
//
//  Author: N.H. Elroy with support from Claude Code (Sonnet)
//
//
//=======================================================================

#include "SharedSettings.hpp"
#include "ModelDownloadDialog.hpp" // runOnUiThreadLater
#include "WhisperFilter.hpp"       // WhisperFilter
#include "WhisperEngine.hpp"       // startWhisper, isAppleSilicon

#include <obs-module.h>
#include <util/platform.h>

#include <algorithm>
#include <fstream>
#include <mutex>
#include <vector>

namespace {
std::mutex filtersMtx;
std::vector<WhisperFilter *> filterList;
} // namespace

void registerFilter(WhisperFilter *filter)
{
	std::lock_guard<std::mutex> lk(filtersMtx);
	filterList.push_back(filter);
}

void unregisterFilter(WhisperFilter *filter)
{
	std::lock_guard<std::mutex> lk(filtersMtx);
	filterList.erase(std::remove(filterList.begin(), filterList.end(), filter), filterList.end());
}

bool filterStillRegistered(WhisperFilter *filter)
{
	std::lock_guard<std::mutex> lk(filtersMtx);
	return std::find(filterList.begin(), filterList.end(), filter) != filterList.end();
}

WhisperFilter *firstRegisteredFilter()
{
	std::lock_guard<std::mutex> lk(filtersMtx);
	return filterList.empty() ? nullptr : filterList.front();
}

static std::string sharedSettingPath(const char *name)
{
	char *dir = obs_module_config_path(nullptr);
	if (!dir)
		return {};
	std::string path(dir);
	bfree(dir);
	if (!path.empty() && path.back() != '/' && path.back() != '\\')
		path += '/';
	path += name;
	return path;
}

static std::string readStoredSetting(const char *name)
{
	const std::string path = sharedSettingPath(name);
	if (path.empty())
		return {};
	std::ifstream in(path, std::ios::binary);
	if (!in)
		return {};
	std::string value((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	while (!value.empty() &&
	       (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' || value.back() == '\t'))
		value.pop_back();
	return value;
}

static void writeStoredSetting(const char *name, const std::string &value)
{
	if (value.empty())
		return;
	char *dir = obs_module_config_path(nullptr);
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	const std::string path = sharedSettingPath(name);
	if (path.empty())
		return;
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (out)
		out << value;
}

#define STORE_PROCESSING_DEVICE "processing_device"
#define STORE_MODEL_PATH        "model_path"

// In-memory copy of the two plugin-wide settings,available
// as reference to all plugin instances via the read and write functions
namespace {
bool deviceCacheLoaded = false;
std::string cachedDevice;
bool modelCacheLoaded = false;
std::string cachedModel;
} // namespace

std::string readStoredProcessingDevice()
{
	if (!deviceCacheLoaded) {
		cachedDevice = readStoredSetting(STORE_PROCESSING_DEVICE);
		deviceCacheLoaded = true;
	}
	return cachedDevice;
}

void writeStoredProcessingDevice(const std::string &device)
{
	writeStoredSetting(STORE_PROCESSING_DEVICE, device);
	cachedDevice = device;
	deviceCacheLoaded = true;
}

std::string readStoredModelPath()
{
	if (!modelCacheLoaded) {
		cachedModel = readStoredSetting(STORE_MODEL_PATH);
		modelCacheLoaded = true;
	}
	return cachedModel;
}

void writeStoredModelPath(const std::string &path)
{
	writeStoredSetting(STORE_MODEL_PATH, path);
	cachedModel = path;
	modelCacheLoaded = true;
}

/*
    This is key to managing the fact that each filter instance can try and change the
    stored settings. This function is a decision table on what to do when an attempt
    is made to change shared settings. The inputs are:
 
    - haveStored: the plugin config file for this setting exists and is non-empty.
    - seenBefore: this filter has already been through reconciliation once. False means
      the filter is loading from a saved scene collection. True means the streamer is editing it live.
    - panelHasUserValue: OBS reports the panel value was explicitly set at some point, not just a default.
    - panelMatchesStored: the panel value equals the stored value.
 
    See definition of SharedAction for results
 */
SharedAction reconcileShared(bool haveStored, bool seenBefore, bool panelHasUserValue, bool panelMatchesStored)
{
	if (!seenBefore) {
		if (haveStored)
			return panelMatchesStored ? SharedAction::None : SharedAction::Adopt;
		if (panelHasUserValue)
			return SharedAction::Donate;
		return SharedAction::ComputeDefault;
	}
	return panelMatchesStored ? SharedAction::None : SharedAction::Publish;
}

/*
    Filters will all need to be reloaded when a key shared setting is updated.
    The master filter will also need to reload the model, which takes a long
    time. Because this can be called from obs_source_update () the restart of
    the filter needs to be deferred in order to avoid a race condition
    requestReload() is the only caller, which queues this function
 */
static void reloadFilterTask(void *param)
{
	auto *filter = static_cast<WhisperFilter *>(param);

	// If filter is still registered then the pointer will still be valid
	// Check needed because filter may have been deleted
	if (!filterStillRegistered(filter)) {
		blog(LOG_INFO, "[babelstreamer-filter] Skipping a queued rebuild for a filter that is gone");
		return;
	}

	filter->reloadQueued = false;
	if (filter->modelPath.empty())
		return;

	std::lock_guard<std::mutex> lock(filter->whisperMtx);
	filter->whisper.reset(); // stop() runs in the destructor
	filter->resampleAccum = 0.0f;
	startWhisper(filter); // picks up the current settings via makeLiveConfig()
}

/*
    Reload of filter deferred. Check that a reload isn't already in
    propgress before triggering
*/
void requestReload(WhisperFilter *filter)
{
	if (filter->reloadQueued)
		return;
	filter->reloadQueued = true;
	runOnUiThreadLater([filter] { reloadFilterTask(filter); });
}

/*
    Rebuild all filters. Needed when a shared resource (e.g. the model)
    is changed by the user and all filters need to conform to the change
 */
void requestReloadAll()
{
	std::vector<WhisperFilter *> all;
	{
		std::lock_guard<std::mutex> lk(filtersMtx);
		all = filterList;
	}
	for (WhisperFilter *f : all)
		requestReload(f);
}

/*
    Which processing device are we using? If one is already cached
    then use that, otherwise take the default and cache it.
 */
std::string resolveProcessingDevice()
{
	const std::string stored = readStoredProcessingDevice();
	if (!stored.empty())
		return stored;
#if HAS_WHISPER_COREML
	// On Apple Silicon default to the Neural Engine
	if (isAppleSilicon()) {
		writeStoredProcessingDevice(ANE_DEVICE_ID);
		return ANE_DEVICE_ID;
	}
#endif
	// Everywhere else prefer the best real GPU, so a machine with one
	// transcribes on it out of the box.
	const std::string preferred = WhisperWrapper::resolveAutoDeviceId();
	const std::string chosen = preferred.empty() ? DEFAULT_GPU_DEVICE : preferred;
	writeStoredProcessingDevice(chosen);
	return chosen;
}

/*
    Publish a new device choice and rebuild every OTHER loaded filter. The
    master filter is skipped: filterUpdate() reloads it through its own
    reloadNeeded path, applying all of its other pending settings at the same time.
 */
void applyProcessingDeviceToAll(const std::string &device, WhisperFilter *origin)
{
	writeStoredProcessingDevice(device);

	std::vector<WhisperFilter *> otherFilters;
	{
		std::lock_guard<std::mutex> lk(filtersMtx);
		for (WhisperFilter *other : filterList)
			if (other != origin && other->gpuDeviceId != device)
				otherFilters.push_back(other);
	}
	if (otherFilters.empty())
		return;

	blog(LOG_INFO, "[babelstreamer-filter] Processing device is now '%s' - reloading %zu other filter(s)",
	     device.c_str(), otherFilters.size());

	for (WhisperFilter *otherFilter : otherFilters) {
		otherFilter->gpuDeviceId = device;
		// Keep that filter's saved settings in step for its properties panel
		if (obs_data_t *otherSettings = obs_source_get_settings(otherFilter->context)) {
			obs_data_set_string(otherSettings, S_GPU_DEVICE, device.c_str());
			obs_data_release(otherSettings);
		}
		requestReload(otherFilter);
	}
}

/*
    Set a new language for all other filters. Each filter can have its
    own source language, but the exception is when the model is English
    only when all filters are forced to English
 */
void applySourceLangToAll(const std::string &lang, WhisperFilter *masterFilter)
{
	std::vector<WhisperFilter *> otherFilters;
	{
		std::lock_guard<std::mutex> lock(filtersMtx);
		for (WhisperFilter *otheFilter : filterList)
			if (otheFilter != masterFilter && otheFilter->sourceLang != lang)
				otherFilters.push_back(otheFilter);
	}
	if (otherFilters.empty())
		return;

	blog(LOG_INFO,
	     "[babelstreamer-filter] Speech language is now '%s' on %zu other filter(s) - "
	     "the shared model only supports that one",
	     lang.c_str(), otherFilters.size());
	for (WhisperFilter *other : otherFilters) {
		other->sourceLang = lang;
		// Same as the model/device publishes: update their saved settings so the
		// panel matches, and reload directly rather than re-entering filterUpdate.
		if (obs_data_t *otherSettings = obs_source_get_settings(other->context)) {
			obs_data_set_string(otherSettings, S_SOURCE_LANG, lang.c_str());
			obs_data_release(otherSettings);
		}
		requestReload(other);
	}
}

/*
    Set a new model for all other filters. Filters use a single model for all
    instances, so you just need to tell them to use the new model. The masterFilter
    is the only one that actually load the model
 */
void applyModelToAll(const std::string &modelPath, WhisperFilter *masterFilter)
{
	writeStoredModelPath(modelPath);

	std::vector<WhisperFilter *> otherFilters;
	{
		std::lock_guard<std::mutex> lockk(filtersMtx);
		for (WhisperFilter *otherFilter : filterList) {
			if (otherFilter != masterFilter && otherFilter->modelPath != modelPath) {
				otherFilters.push_back(otherFilter);
			}
		}
	}
	if (otherFilters.empty())
		return;

	blog(LOG_INFO, "[babelstreamer-filter] Model is now '%s' - reloading %zu other filter(s)", modelPath.c_str(),
	     otherFilters.size());

	for (WhisperFilter *other : otherFilters) {
		other->modelPath = modelPath;
		// Keep that filter's saved settings in step for the properties panel
		if (obs_data_t *otherSettings = obs_source_get_settings(other->context)) {
			obs_data_set_string(otherSettings, S_MODEL_PATH, modelPath.c_str());
			obs_data_release(otherSettings);
		}
		requestReload(other);
	}
}
