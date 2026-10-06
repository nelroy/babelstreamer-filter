// SPDX-FileCopyrightText: 2026 Kamayu B.V. <https://www.kamayu.nl>
// SPDX-License-Identifier: GPL-2.0-or-later

//=======================================================================
//
//  ModelDownloadDialog.hpp
//
//  Created: 20 sep 2026
//
//  Author: N.H. Elroy with support from Claude Code (Sonnet)
//
///  Abstract:       definitions for dialogue to download Whisper language model
///            The process of downloading a language model is automated here.
///            The models are downloaded from the hugging face whisper repository
///            and are:
///
///              - If 'English only' is chosen then "ggml-medium.en.bin"
///              - If multilingual chosen the "ggml-large-v3-q5_0.bin"
///              - If the device is a Neural engine then the appropriate matching
///                neural engine precoder
///
///            The fact that the Mac has a neural engine which requires a
///            separate file complicates things. You have to check that the
///            appropriate sister file is available. The download of the neural
///            engine model is also triggered when the user selects a model
///            manually when using the Neural Engine, or selects the Neural
///            Engine when using CPU or GPU. On Windows machines this is not
///            an issue.
//
//=======================================================================
#pragma once

#include <functional>
#include <string>
#include <vector>

/**
    Structure to define the type of processing device the model is to run on. Currently:
    - GPU
    - CPU
    - Apple Neural Engine
 
 */
struct ProcessingDeviceOption
{
	std::string id;    /*! id for device */
	std::string label; /*! label for device*/
};

/**
    Structure for storing outcome of standard model choice
 */
struct StandardModelOutcome
{
	bool accepted = false;    /*! false = they backed out before anything was fetched */
	bool englishOnly = false; /*! they picked the English-only model */
	std::string deviceId;     /*! the processing device they picked */
	std::string modelPath;    /*! the downloaded .bin — empty if it failed or was cancelled */
};

/**
    Download a standard model from Hugging Face. Must be called from the Qt UI thread, and NOT from inside
    one of OBS's own widget slots — see runOnUiThreadLater above. Blocks on its own modal dialogs until
    finished or cancelled.
 
    @param devices                  list of devices that can host model
    @param currentDevice       current device to run model
    @param neuralDeviceId      id of the neural engine device
    @param explainFirst           if true the dialogue should show an explainer text
    @param onComplete          completion callback. Returns @see StandardModelOutcome
 
 */
void downloadStandardModelGuided(const std::vector<ProcessingDeviceOption> &devices, const std::string &currentDevice,
				 const std::string &neuralDeviceId, bool explainFirst,
				 std::function<void(const StandardModelOutcome &)> onComplete);

/**
    Helper to run a function on the Qt UI thread, off the caller's stack.

    Posts fn with an explicit Qt::QueuedConnection, so it NEVER runs inline
    not even when the caller is already on the UI thread. It is explicitly
    delayed so that it doesn't run immediately if called from the UI thread


    @param fn      the function to run;
 */
void runOnUiThreadLater(std::function<void()> fn);

/**
    Outcome of fetching a Core ML encoder.
 */
enum class CoremlEncoderResult
{
	Installed,    /*! downloaded (or already present) and ready to use */
	NotAvailable, /*! no published encoder for this model — not an error */
	Cancelled,    /*! the streamer backed out */
	Failed,       /*! download, integrity check, or extraction failed */
};

/**
    Fetch the Apple Neural Engine (Core ML) encoder for a specific whisper model, verifies it, unzips it into
    the plugin's coreml cache (<plugin config>/coreml/), then warm-compiles it for this Mac's ANE in
    a modal so the first real transcription isn't burdened by the compile on the UI thread. Apple-Silicon only —
    returns NotAvailable immediately elsewhere.
 
    The name of the Core ML encoder is derived from the name of the language model (@see coremlEncoderNameFor)
    The standard encoder, as the model, is fetched from a pinned commit as we are using the content hash hard coded to
    validate. If it's a non-standard model we just use the latest commit. An encoder already in the cache reports Installed
    immediately without asking or fetching.
 
    Like downloadStandardModel(), must be called from the Qt UI thread and blocks on its own modal dialogs until
    finished or cancelled.

    @param modelPath        path to whisper model for which the Core ML encoder is required
    @param askFirst            if true the user is first asked if they want to do the download
    @param onComplete     async completion callback. Fills a CoremlEncoderResult structure
 */
void downloadCoremlEncoderFor(const std::string &modelPath, bool askFirst,
			      std::function<void(CoremlEncoderResult)> onComplete);


/**
    Derive the name of a Core ML encoder model from the whisper language model. For example the
    core ML encoder for ggml-medium-en.bin is ggml-medium-en-encoder.mlmodelc and these conventions
    are followed in the repo.
    
    @param modelPath   path to Whisper model
    
    @returns path to Core ML encoder (may or may not exist)
 */
std::string coremlEncoderNameFor(const std::string &modelPath);

/**
    True when an encoder for modelPath|is already installed in the plugin's   coreml cache. (A real encoder the user
    placed next to the model themselves is handled separately by ensureCoremlAdjacency).
 
    @param modelPath   path for model
 
    @returns true if encoder already cached
 */
bool coremlEncoderCached(const std::string &modelPath);
