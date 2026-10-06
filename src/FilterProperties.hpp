// SPDX-FileCopyrightText: 2026 Kamayu B.V. <https://www.kamayu.nl>
// SPDX-License-Identifier: GPL-2.0-or-later

//=======================================================================
//
//  FilterProperties.hpp
//
//  Created: 26 sep 2026
//
//  Author: N.H. Elroy with support from Claude Code (Sonnet)
//
///  Abstract: The OBS properties-panel presenter for the BabelStreamer
///          filter — dialog construction, its button/dropdown callbacks,
///          the guided model-download and Neural Engine prompt flows, the
///          account-pairing flow, the device-token sidecar cache, and
///          filterUpdate() itself (the settings-apply counterpart to the
///          panel this file presents).
//
//=======================================================================

#pragma once

#include <obs-module.h>
#include <string>

struct WhisperFilter;

/**
    Called by OBS to set up the default filter settings
 
    @param settings - OBS passes this structure to contain the settings
    
 */
void filterDefaults(obs_data_t *settings);

/**
    Called by OBS to build the properties dialogue
 
    @param data Whisper Filter structure which needs to be cast to use
 */
obs_properties_t *filterProperties(void *data);

/**
    If no model set offer to download a model via a dialogue and download
    it if the user agrees
 
    @param filter filter that is calling this
 */
void offerModelDownload(WhisperFilter *fiter);

/**
    If no account is linked (e.g. via cached token) then show a dialogue
    that offers to link an account and communicate with the server to do this.
    You call this without knowing the account status as the function does its own check
 */
void maybeOfferAccountLink();

/**
    OBS calles this to update the filter settings
 
    @param data Whisper Filter structure which needs to be cast to use
    @param settings filter settings in OBS format
    
 */
void filterUpdate(void *data, obs_data_t *settings);

/**
    Arm the frontend-ready flag that keeps modal dialogsout of OBS's own startup. 
 */
void watchFrontendReady();

/**
    Disarm the frontend-ready flag.
 */
void unwatchFrontendReady();
