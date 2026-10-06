// SPDX-FileCopyrightText: 2026 Kamayu B.V. <https://www.kamayu.nl>
// SPDX-License-Identifier: GPL-2.0-or-later

//=======================================================================
//
//  plugin-main.cpp
//
//  Created: 20 sep 2026
//
//  Author: N.H. Elroy with support from Claude Code (Sonnet)
//
//  Abstract: interface to OBS
//
//=======================================================================

#include "WhisperFilter.hpp"
#include <plugin-support.h>

#include "FilterProperties.hpp"
#include "UpdateChecker.hpp"
#include "GameVocab.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("babelstreamer-filter", "en-US")

MODULE_EXPORT const char *obs_module_description()
{
	return "Transcribes audio via whisper.cpp and streams results as JSON "
	       "over a WebSocket to a translation/broadcast server.";
}

/**
    Main loader for the OBS plugin. As well as registering with OBS the latest game vocab
    is downloaded from the server and a check is done for an updated version of the plugin
 
 */
bool obs_module_load()
{
	blog(LOG_INFO, "[babelstreamer-filter] Loading plugin v%s", PLUGIN_VERSION);
	obs_register_source(getWhisperFilterInfo());
	blog(LOG_INFO, "[babelstreamer-filter] Registered 'BabelStreamer Speech Filter' audio filter");
	watchFrontendReady();    // when ready start the whole thing up
	checkForPluginUpdate();  // check if there is a new version of the plugin available
	refreshGameVocabulary(); // download updated per-game vocab in the background
	return true;
}

void obs_module_unload()
{
	unwatchFrontendReady();
	blog(LOG_INFO, "[babelstreamer-filter] Unloaded");
}
