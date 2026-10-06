// SPDX-FileCopyrightText: 2026 Kamayu B.V. <https://www.kamayu.nl>
// SPDX-License-Identifier: GPL-2.0-or-later

//=======================================================================
//
//  SharedSettings.hpp
//
//  Created: 26 sep 2026
//
//  Author: N.H. Elroy with support from Claude Code (Sonnet)
//
///  Abstract: The processing device and model path are plugin-wide rather reconciling a filter's
///         panel against them, propagating a change to every other loaded filter, and the
///         registry of  loaded filters that propagation needs.
//
//=======================================================================

#pragma once

#include <string>

struct WhisperFilter;

/**
    Register a newly created filter. Called once, from filterCreate().
 
    @param f filter to register
 */
void registerFilter(WhisperFilter *f);

/**
    Unregister a filter being destroyed. Called once, from filterDestroy().
 
    @param f filter to destroy
 */
void unregisterFilter(WhisperFilter *f);

/**
    Whether a filter is still registered. A pointer-only comparison — never
    dereferences f — so it's a safe way for a deferred UI task to check the
    filter it captured wasn't destroyed while the task was queued.
 
    @param f filter to check
 
    @returns true if still registered
 */
bool filterStillRegistered(WhisperFilter *f);

/**
    Any one currently registered filter, or nullptr if none are loaded. Used to
    borrow a filter's server address for a plugin-wide action (account pairing)
    that isn't really about any single filter.
 
    @returns registered filter or nullptr
 */
WhisperFilter *firstRegisteredFilter();

/**
 
    Queue f's whisper context to be rebuilt on the UI thread. Collapses repeats.
 
    @param f filter to queue
 */
void requestReload(WhisperFilter *f);

/**
    requestReload() for every registered filter.
 */
void requestReloadAll();

enum class SharedAction
{
	None,           /*! panel already agrees with the plugin-wide value */
	Adopt,          /*! take the stored value and write it back into this panel */
	Donate,         /*! nothing stored yet; this filter's saved choice becomes it */
	ComputeDefault, /*! nothing stored and nothing saved: caller works one out */
	Publish,        /*! the streamer picked something here; push it to every filter */
};

/**
    Reconcile shared actions to a single global action (see def of SharedAction)
 
    @param haveStored
    @param seenBefore
    @param panelHasUserValue
    @param panelMatchesStored
 
    @returns reconciled shared action
 */
SharedAction reconcileShared(bool haveStored, bool seenBefore, bool panelHasUserValue, bool panelMatchesStored);

// Processing device (plugin-wide)
std::string readStoredProcessingDevice();
void writeStoredProcessingDevice(const std::string &device);
std::string readStoredModelPath();
void writeStoredModelPath(const std::string &path);

/**
    The device the whole plugin is using. An already-stored value wins;
    otherwise the first-run default is computed ONCE and stored, so it's a
    stable, inspectable choice from then on rather than a mode that
    re-resolves on every reload.
 
    @returns name of processing device
 */
std::string resolveProcessingDevice();

/**
    Publish a new device choice and rebuild every OTHER loaded filter on it.
 
    @param device device to use
    @param origin filter that has updated the device
 */
void applyProcessingDeviceToAll(const std::string &device, WhisperFilter *origin);

/**
    Force one speech language onto every OTHER loaded filter. Language is
    normally per-filter, so this is ONLY for the case where the plugin-wide
    model settles it (an English-only .bin cannot transcribe anything else).
 
    @param lang language to use
    @param origin filter that has updated the language
 */
void applySourceLangToAll(const std::string &lang, WhisperFilter *origin);

/**
    Publish a new model choice and rebuild every OTHER loaded filter on it.
 
    @param modelPath path to new model
    @param origin filter that has updated the model
 */
void applyModelToAll(const std::string &modelPath, WhisperFilter *origin);
