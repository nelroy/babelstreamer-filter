// SPDX-FileCopyrightText: 2026 Kamayu B.V. <https://www.kamayu.nl>
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
//=======================================================================
//
//  WhisperFilter.hpp
//
//  Created: 20 sep 2026
//
//  Author: N.H. Elroy with support from Claude Code (Sonnet)
//
///  Abstract: Body of the OBS audio filter for whisper. Captures audio
///            resamples 20 16KHz, recognises and optionally sends to
///            the server as JSON. Also handles settings panel
///
///      Pipeline:
///
///          OBS audio callback (any rate, any channel count)
///               → downmix all channels to mono
///               → linear-interpolation resample to 16 kHz float32
///               → WhisperWrapper (Silero VAD + whisper.cpp transcription thread)
///               → outbox (bounded queue) → sender thread → WebSocket JSON payload
///               → Node.js translation server
///
///
//=======================================================================

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include <obs-module.h>

#include "ServerLink.hpp"
#include "WhisperWrapper.h"

// Settings keys
#define S_MODEL_PATH         "model_path"           /*! path to whisper model */
#define S_WS_HOST            "ws_host"              /*! host name/IP */
#define S_WS_PORT            "ws_port"              /*! host port */
#define S_WS_USE_TLS         "ws_use_tls"           /*! use encrypeted flag*/
#define S_DEVICE_TOKEN       "device_token"         /*! account token to connect device*/
#define S_SOURCE_LANG        "source_lang"          /*! spoken source language*/
#define S_CUSTOM_VOCAB_FILE  "custom_vocab_file"    /*! filename for custom vocB*/
#define S_GAME_PRESET        "game_preset"          /*! predefined game choice for extra vocab*/

// Silero speech detection
#define S_VAD_THRESHOLD      "vad_threshold"        /*! threshhold for speech detection in %*/
#define S_GPU_DEVICE         "gpu_device"           /*! GPU (or CPUo Neural Engine) to use*/
#define ANE_DEVICE_ID        "coreml"               /*! standard processing device for Apple Silicon*/
#define S_VAD_FRAME_MS       "vad_frame_ms"         /*! frame size for speech detection */
#define S_VAD_SPEECH_MS      "vad_speech_ms"        /*! frame size for accepting as speech */
#define S_VAD_SILENCE_MS     "vad_silence_ms"       /*! silence length to determine end of utterance */
#define S_MAX_UTTERANCE_MS   "max_utterance_ms"     /*! max cuttoff for long utterance */
#define S_PARTIAL_EVERY_MS   "partial_every_ms"     /*! send partials every .. 0 = never */
#define S_PARTIAL_WINDOW_MS  "partial_window_ms"    /*! length of partial */
#define S_SPEAKER_NAME       "speaker_name"         /*! name of speaker for multi input speaker tagging */
#define S_TEXT_SOURCE_NAME   "text_source_name"     /*! OBS text field to rendeer captions*/
#define S_SHOW_PARTIALS      "show_partials"        /*! not displayed - show partial transcriptions*/
#define S_SESSION_KEY        "session_key"          /*! babelstreamer translation session key   */
#define S_PLUGIN_VERSION     "plugin_version"       /*! version */
#define S_STREAM_GATE_ENABLED "stream_gate_enabled" /*! only connect to server when streaming */

// Default values
#define DEFAULT_WS_HOST           "babelstreamer.com"
#define DEFAULT_WS_PORT           443
#define DEFAULT_WS_USE_TLS        true
#define DEFAULT_SOURCE_LANG       "en"
#define DEFAULT_GPU_DEVICE        "cpu"
#define DEFAULT_VAD_FRAME_MS      20
#define DEFAULT_VAD_SPEECH_MS     120
#define DEFAULT_VAD_SILENCE_MS    260
#define DEFAULT_MAX_UTTERANCE_MS  10000 // note: whisper's own window is 30s, but this is too long
#define DEFAULT_PARTIAL_EVERY_MS  0
#define DEFAULT_PARTIAL_WINDOW_MS 2500
#define DEFAULT_VAD_THRESHOLD_PCT 30 // Silero's default is 50, but tests show that this works better
#define DEFAULT_STREAM_GATE_ENABLED true
#define VAD_MODEL_FILENAME   "ggml-silero-v6.2.0.bin" // Silero ships with the app and is saved to this file

/**
    The actual filter structure. This is instantiated for each audio source. The plumbing for ensuring
    that we only use on instance of the model is in WhisperWrapper. There is one master filter which
    controls the single instance of language models and server link and ensures that the other instances
    are kept in step

    Three threads touch this struct:
    - UI thread: filterUpdate/filterProperties/filterCreate/filterDestroy: the "enable" signal handler
    - WhisperWrapper worker:the transcription callback (overlay update + transcript enqueue)
    - audio thread: filterAudio
 
    Fields crossing those threads are guarded by stateMtx below. Fields NOT so marked are only touched from
    the UI thread (or handed to whisper via Config/callback-capture at rebuild time) and need no lock.
 */
struct WhisperFilter
{
	obs_source_t *context = nullptr;

	std::unique_ptr<WhisperWrapper> whisper; /*! the actuel model */
	std::mutex whisperMtx;                   /*! protects whisper during reload */
	uint64_t linkId = 0;                     /*! handle to register with global ServerLink oconnection handler */
	std::mutex stateMtx;                     /*! guards the cross-thread fields marked below */

	/**
        Stored settings (used to detect changes in filterUpdate)
        we need to keep track of this becuase OBS only tells you that settings have changed. Since some settings result in a complete teardown
        and rebuild of the model, which is slow, we need to keep a shadow copy to check what is changed.
     */
	std::string modelPath;                        /*! shadow model path */
	bool modelChoiceSeen = false;                 /*! false until a model is actually chosen*/
	bool reloadQueued = false;                    /*! reload of model queued, don't need to check the rest*/
	std::string anePairChecked;                   /*! Which model/device pair was checked for Apple neural engine, assures that prompt is once per choice */
	bool anePairPrimed = false;                   /*! that the ANE choice been looked at at all*/
	bool anePromptQueued = false;                 /*! true when a prompt to load ANE has been queueud*/
	bool aneRevertDevice = false;                 /*! which fallback to use if user refuses ANE LOAD*/
	std::string anePrevModel;                     /*! previously loaded ANE model*/
	std::string anePrevDevice;                    /*! previously loaded ANE device*/
	std::string wsHost = DEFAULT_WS_HOST;         /*! sockets host, guarded by stateMtx (read by sender) */
	uint16_t wsPort = DEFAULT_WS_PORT;            /*! sockets port,  guarded by stateMtx (read by sender) */
	bool wsUseTls = DEFAULT_WS_USE_TLS;           /*! use secure connection, guarded by stateMtx (read by sender) */
	std::string sourceLang = DEFAULT_SOURCE_LANG; /*! speech source language */
	std::string customVocab;                      /*! user custom vocabulary/name hints */

	/*! Mirrors the PLUGIN-WIDE processing device (see resolveProcessingDevice in
        SharedSettings.cpp), not an independent per-filter choice — the Core ML
        encoder symlink is keyed on the model path, so filters sharing a model
        can't run different devices without fighting over it.
     */
	std::string gpuDeviceId = DEFAULT_GPU_DEVICE; /*! chosen GPU, see note above */
	bool deviceChoiceSeen = false;                /*! false until a device choice is  made*/
	int vadFrameMs = DEFAULT_VAD_FRAME_MS;
	int vadSpeechMs = DEFAULT_VAD_SPEECH_MS;
	int vadSilenceMs = DEFAULT_VAD_SILENCE_MS;
	int maxUtteranceMs = DEFAULT_MAX_UTTERANCE_MS;
	int partialEveryMs = DEFAULT_PARTIAL_EVERY_MS;
	int partialWindowMs = DEFAULT_PARTIAL_WINDOW_MS;
	int vadThresholdPct = DEFAULT_VAD_THRESHOLD_PCT; // 0-100, /100 for the wrapper
	
    std::string speakerName;    /*! name of speaker for this instance, guarded by stateMtx*/
	std::string textSourceName; /*! name of text source to drive for this instance, guarded by stateMtx */
	bool showPartials = false;  /*! show partial results in [brackets] - NOTE: this is marked for deprecation */
	std::string gamePreset;     /*! mirrors S_GAME_PRESET (*/
	std::string deviceToken;    /*! token to link to account, guarded by statMtx*/
	float resampleAccum = 0.0f; /*! resample carry-over*/

	/*!< stream gate: don't transcribe when not streaming. Use atomic for low cost update*/
	std::atomic<bool> streamGateEnabled{DEFAULT_STREAM_GATE_ENABLED};
};

/**
    Interface for OBS to get inf

    @returns info for OBS
 */
obs_source_info *getWhisperFilterInfo();

/**
    Finds any source on the canvas by name and sets its "text" property. Safe
    to call from any thread. Takes the source NAME rather than the filter
    struct: callers on the whisper worker thread pass a snapshot taken under
    stateMtx, so this never reads f->textSourceName concurrently with a
    filterUpdate() write on the UI thread. Used by WhisperEngine.cpp's
    startWhisper() callback as well as this file.
 */
void updateTextSource(const std::string &sourceName, const std::string &text);

/**
    Snapshot of f's contribution to the shared connection (ServerLink). Used
    by filterCreate/filterDestroy here and by filterUpdate() in
    FilterProperties.cpp, which is why it isn't static.
 */
ServerLinkParams makeLinkParams(WhisperFilter *f);
