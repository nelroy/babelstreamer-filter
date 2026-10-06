// SPDX-FileCopyrightText: 2026 Kamayu B.V. <https://www.kamayu.nl>
// SPDX-License-Identifier: GPL-2.0-or-later

//=======================================================================
//
//  WhisperEngine.cpp
//
//  Created: 26 sep 2026
//
//  Author: N.H. Elroy with support from Claude Code (Sonnet)
//
//  Dev note:  Currently partial captions are supported. For
//             translation this is undesirable because (a) it uses
//             a lot more GPU and (b) not needed because of effective
//             zero latency. However if the plugin is only used for captions
//             it may be useful. Decide later if needed
//
//=======================================================================

#include "WhisperEngine.hpp"
#include "ModelDownloadDialog.hpp" // coremlEncoderNameFor's declaration lives here
#include "ServerLink.hpp"          // serverLinkEnqueue
#include "WhisperFilter.hpp"       // WhisperFilter, updateTextSource, settings keys

#include <obs-module.h>
#include <util/platform.h>

#include <mutex>
#include <regex>
#include <sstream>

#if HAS_WHISPER_COREML
#include <sys/sysctl.h>
#endif

/*
    Build a JSON string t=in the format expected by the server whisperReceiver. Format is:

    { "type": "transcript", "text": "...", "is\_final": true, "lang": "en", "game": "", "start\_ms": 12345, "end\_ms": 13890 }

    Note that the "game" field is used by the translator module top filter out game-specific vocab that
    does not translate. Timings are WhisperWrappers own clock and purely relative.

    text:        recognised text to send
    isFinal:    text is final utterance
    lang:       language of speaker
    game:       optional chosen game for extra vocab
    speaker:    speaker name
    startMS:    start of utterance
    end MS:     end of utterance

    returns formatted JSON string

 */
static std::string makeJson(const std::string &text, bool isFinal, const std::string &language, const std::string &game,
			    const std::string &speaker, int64_t startMs, int64_t endMs)
{
	// handle escaping of characters
	auto escape = [](const std::string &s) {
		std::string out;
		out.reserve(s.size() + 4);
		for (char nextChar : s) {
			if (nextChar == '"')
				out += "\\\"";
			else if (nextChar == '\\')
				out += "\\\\";
			else if (nextChar == '\n')
				out += "\\n";
			else if (nextChar == '\r')
				out += "\\r";
			else
				out += nextChar;
		}
		return out;
	};

	std::ostringstream result;
	result << "{"
	       << "\"type\":\"transcript\","
	       << "\"text\":\"" << escape(text) << "\","
	       << "\"is_final\":" << (isFinal ? "true" : "false") << ","
	       << "\"lang\":\"" << escape(language) << "\","
	       << "\"game\":\"" << escape(game) << "\"";

	// speaker omitted entirely if not given
	if (!speaker.empty())
		result << ",\"speaker\":\"" << escape(speaker) << "\"";

	if (isFinal) {
		result << ",\"start_ms\":" << startMs << ",\"end_ms\":" << endMs;
	}
	result << "}";
	return result.str();
}

// Live config can be updated without a reload, so you can update this seperately
WhisperWrapper::LiveConfig makeLiveConfig(const WhisperFilter *filter)
{
	WhisperWrapper::LiveConfig live;
	live.vad_frame_ms = filter->vadFrameMs;
	live.vad_min_speech_ms = filter->vadSpeechMs;
	live.vad_end_silence_ms = filter->vadSilenceMs;
	live.max_utterance_ms = filter->maxUtteranceMs;
	live.partial_every_ms = filter->partialEveryMs;
	live.partial_window_ms = filter->partialWindowMs;
	live.initial_prompt = filter->customVocab;
	// UI stores percent; the wrapper compares against a 0..1 probability.
	live.vad_threshold = (float)filter->vadThresholdPct / 100.0f;
	return live;
}

/*
    Derive a CoreML encoder filename from the model filename. Rules are:

        - strip the extension
        - drop a trailing "-q<digit>_<digit>" quantization tag, e.g. "-q5_0"
        - append "-encoder.mlmodelc"

    Note that this implies that quantised and turbo and normal models share an encoder,
    since the Core ML encoder is unaffected by quantisation of the decoder weights.
*/
std::string coremlEncoderNameFor(const std::string &modelPath)
{
	std::string stem = std::filesystem::path(modelPath).filename().string();
	if (auto dot = stem.rfind('.'); dot != std::string::npos)
		stem = stem.substr(0, dot);

	static const std::regex kQuantTag(R"(-q\d_\d$)");
	stem = std::regex_replace(stem, kQuantTag, "");

	return stem + "-encoder.mlmodelc";
}

/*  
    whisper.cpp has no runtime switch to select CoreML. Whisper will user
    CoreML if the build linked CoreML (HAS_WHISPER_COREML) and a compiled
    model exists at whisper's derived path, "<model>-encoder.mlmodelc",
    next to the .bin (see whisper_get_coreml_path_encoder in src/whisper.cpp). So
    the "Use Neural Engine" toggle works by placing - or removing - a symlink at
    that derived path pointing into our own encoder cache.
 */
#if HAS_WHISPER_COREML
// Apple Silicon implies Apple Neural Engine
bool isAppleSilicon()
{
	static const bool v = [] {
		int val = 0;
		size_t sz = sizeof(val);
		if (sysctlbyname("hw.optional.arm64", &val, &sz, nullptr, 0) != 0)
			return false;
		return val != 0;
	}();
	return v;
}

// Get the path for the core ML encoder model
std::filesystem::path coremlAdjacentPath(const std::string &modelPath)
{
	return std::filesystem::path(modelPath).parent_path() / coremlEncoderNameFor(modelPath);
}

namespace {

// This is where we cache our encoders
std::filesystem::path coremlCachedPath(const std::string &modelPath)
{
	std::filesystem::path dir;
	if (char *c = obs_module_config_path("coreml")) {
		dir = c;
		bfree(c);
	}
	return dir / coremlEncoderNameFor(modelPath);
}

/*
    Make sure we have a symlink for the ANE encoder in an adjacent directory,
    assuming that the model is actually available. We use the symlink to
    avoid copying a large file to where it is needed. Whisper derives its
    own modelpath so want something there, but we can't control where the user
    decides to store stuff. Of course if the user put the model where Whisper
    expects it, them we don't need the symlink
 
    Returns true if all OK to use ANE
 */
bool ensureCoremlAdjacency(const std::string &modelPath, bool wantAne)
{
	if (modelPath.empty())
		return false;

	// make sure that two filters trying this don't race
	static std::mutex adjacencyMtx;
	std::lock_guard<std::mutex> adjacencyLk(adjacencyMtx);
	std::error_code error;

	const auto adjacent = coremlAdjacentPath(modelPath);
	const auto cached = coremlCachedPath(modelPath);

	const bool haveCache = std::filesystem::exists(cached, error);
	const bool adjIsSymlink = std::filesystem::is_symlink(adjacent, error);
	const bool adjExists = adjIsSymlink || std::filesystem::exists(adjacent, error);

	if (wantAne && haveCache) {
		if (adjExists && !adjIsSymlink) {
			// A real encoder the user dropped in - respect it, don't override.
			blog(LOG_INFO, "[babelstreamer-filter] Core ML: using existing encoder at %s",
			     adjacent.string().c_str());
			return true;
		}

		if (adjIsSymlink)
			std::filesystem::remove(adjacent, error);

		std::filesystem::create_directory_symlink(cached, adjacent, error);
		if (error) {
			blog(LOG_WARNING,
			     "[babelstreamer-filter] Core ML: could not link encoder (%s) - "
			     "falling back to the CPU/Metal encoder",
			     error.message().c_str());
			return false;
		}
		blog(LOG_INFO,
		     "[babelstreamer-filter] Core ML/ANE encoder ENABLED (%s). First load "
		     "compiles the model for the Neural Engine and can take up to ~30s.",
		     adjacent.string().c_str());
		return true;
	}

	// ANE off, or no cached encoder: make sure OUR symlink isn't left behind
	if (adjIsSymlink) {
		std::filesystem::remove(adjacent, error);
		blog(LOG_INFO, "[babelstreamer-filter] Core ML/ANE encoder disabled (removed %s)",
		     adjacent.string().c_str());
	}
	return false;
}

} // namespace
#endif // HAS_WHISPER_COREML

/*
    The device id actually handed to WhisperWrapper - and therefore the one the
    shared-model registry is keyed on. "Apple Neural Engine" is not a ggml
    backend: the encoder runs on ANE and the decoder runs on ths CPU. For the UI
    this is confusing so we normalis it. So for the UI we are using ANE but for
    whisper this is effectively CPU
 */
std::string effectiveDeviceId(const WhisperFilter *f)
{
#if HAS_WHISPER_COREML
	if (isAppleSilicon() && f->gpuDeviceId == ANE_DEVICE_ID)
		return "cpu";
#endif
	return f->gpuDeviceId;
}

void startWhisper(WhisperFilter *filter)
{
	if (filter->modelPath.empty())
		return;

#if HAS_WHISPER_COREML
	// See note on effectiveDeviceID
	const bool wantAne = isAppleSilicon() && filter->gpuDeviceId == ANE_DEVICE_ID;
#endif

	WhisperWrapper::Config cfg;
	cfg.model_path = filter->modelPath;
	cfg.gpu_device_id = effectiveDeviceId(filter);
	cfg.language = filter->sourceLang;
	cfg.live = makeLiveConfig(filter);

	/*
        Find the Silero model and fallback if unavailable. Log this because
        missing Silero is likely to lead to the model hallucinating and this
        is almost impossible to debug
     */
	if (char *vadPath = obs_module_file(VAD_MODEL_FILENAME)) {
		cfg.vad_model_path = vadPath;
		bfree(vadPath);
	} else {
		blog(LOG_WARNING,
		     "[babelstreamer-filter] Bundled VAD model '%s' is missing from the "
		     "plugin's data directory - falling back to the energy VAD, which cannot "
		     "tell loud non-speech (game audio, music, keyboard) from speech. "
		     "Reinstalling the plugin should restore it.",
		     VAD_MODEL_FILENAME);
	}

#if HAS_WHISPER_COREML
	// Make sure whisper can find the ANE encoder
	ensureCoremlAdjacency(filter->modelPath, wantAne);
#endif

	try {
		/*
            Create the whisper wrapper. The rest is mainly the callback to deal
            with transcribed text. Capture language by value so we don't worry
            about language updates while the filter is decoding.
         */
		filter->whisper = std::make_unique<WhisperWrapper>(cfg, [filter, lang = filter->sourceLang](
										const std::string &text, bool isFinal,
										int64_t startMs, int64_t endMs) {
			// Overlay settings can change without a whisper rebuild, so
			// snapshot them under the lock instead of capturing.
			std::string overlayName;
			bool partialsWanted = false;
			std::string game;
			std::string speaker;
			{
				std::lock_guard<std::mutex> lk(filter->stateMtx);
				overlayName = filter->textSourceName;
				partialsWanted = filter->showPartials;
				game = filter->gamePreset;
				speaker = filter->speakerName;
			}

			// Drive the OBS text source overlay
			if (isFinal) {
				updateTextSource(overlayName, text);
			} else if (partialsWanted) {
				updateTextSource(overlayName, "[" + text + "]");
			}

			// Add a speaker name to the caption, if >1 speakers
			const std::string speakerToSend =
				(ServerLink::getInstance().serverLinkActiveCount() > 1) ? speaker : std::string();

			// Hand off to the sender thread - never any network I/O here.
			ServerLink::getInstance().serverLinkEnqueue(
				makeJson(text, isFinal, lang, game, speakerToSend, startMs, endMs));
			blog(LOG_INFO, "[babelstreamer-filter] [%s] %s", isFinal ? "FINAL" : "partial", text.c_str());
		});
		blog(LOG_INFO, "[babelstreamer-filter] Whisper loaded: %s", filter->modelPath.c_str());
	} catch (const std::exception &e) {
		blog(LOG_ERROR, "[babelstreamer-filter] Failed to load whisper: %s", e.what());
		filter->whisper.reset();
	}
}
