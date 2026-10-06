// SPDX-FileCopyrightText: 2026 Kamayu B.V. <https://www.kamayu.nl>
// SPDX-License-Identifier: GPL-2.0-or-later

//=======================================================================
//
//  WhisperWrapper.hpp
//
//  Created: 20 sep 2026
//
//  Author: N.H. Elroy with support from Claude Code (Sonnet)
//
//  Abstract: Wrapper for Whisper.cpp. Includes speech detection and
//            translation of audio to text
//
//=======================================================================

/*
    General Note: all calls to the backend and the models are bracketed by defensive try/catch blocks
    This is because testing surfaced a number of exceptions that were not caught at lower levels, so
    to ensure that this won't crash OBS we don't allow these exceptions to float up outside of the
    plugin context.
 */

#include "WhisperWrapper.h"
#include "WhisperFilter.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <sstream> // Added include as per instruction

extern "C" {
#include "ggml-backend.h"
}

#if defined(__APPLE__)
extern "C" {
#include "ggml-metal.h"
}
#endif

// HAS_GGML_CUDA, HAS_GGML_HIP and HAS_GGML_VULKAN are set by CMakeLists.txt
// based on the cmake variables exported by ggml-config.cmake.  We do NOT use
// __has_include here because the headers may exist even when the symbols are
// not compiled in.
#ifndef HAS_GGML_CUDA
#define HAS_GGML_CUDA 0
#endif
#ifndef HAS_GGML_HIP
#define HAS_GGML_HIP 0
#endif
#ifndef HAS_GGML_VULKAN
#define HAS_GGML_VULKAN 0
#endif

// ggml's HIP backend is the CUDA backend recompiled with hipcc: same header,
// same ggml_backend_cuda_* symbols, just linked from ggml-hip.lib instead of
// ggml-cuda.lib (see CMakeLists.txt - the two are mutually exclusive).
#if HAS_GGML_CUDA || HAS_GGML_HIP
extern "C" {
#include "ggml-cuda.h"
}
#endif

#if HAS_GGML_VULKAN
extern "C" {
#include "ggml-vulkan.h"
}
#endif

// On Apple we only use Metal (no CUDA / Vulkan)
#ifdef __APPLE__
#undef HAS_GGML_CUDA
#define HAS_GGML_CUDA 0
#undef HAS_GGML_HIP
#define HAS_GGML_HIP 0
#undef HAS_GGML_VULKAN
#define HAS_GGML_VULKAN 0
#endif

// Note: does not include Neural Engine, as this is dealt with separately

static std::string device_id(const BackendChoice &b)
{
	switch (b.kind) {
	case BackendKind::CUDA:
		return "cuda:" + std::to_string(b.device);
	case BackendKind::HIP:
		return "hip:" + std::to_string(b.device);
	case BackendKind::Vulkan:
		return "vulkan:" + std::to_string(b.device);
	case BackendKind::Metal:
		return "metal:" + std::to_string(b.device);
	default:
		return "cpu";
	}
}

/*
    Queries every backend compiled into this build for its available devices.
    Shared by pickBestBackend() (auto-select) and listAvailableDevices()
    (the OBS properties dropdown) so there's one place that knows how to talk
    to each backend's device-enumeration API.
 */
static std::vector<BackendChoice> enumerateGpuDevices()
{
	std::vector<BackendChoice> out;

	// ggml_backend_load_all() LoadLibrary()s every backend DLL present next
	// to the plugin (ggml-vulkan.dll, ggml-cuda.dll, ...); Backends can throw
	// during loading so we need to catch here to avoid crashing OBS.
	try {
		ggml_backend_load_all();
	} catch (const std::exception &e) {
		blog(LOG_WARNING, "[whisper] ggml_backend_load_all() threw, GPU backends may be unavailable: %s",
		     e.what());
	}

	// Each backend can throw when we call it below, so catch these in order to avoid crashing OBS
	// Failing backends are just skipped
#if HAS_GGML_CUDA || HAS_GGML_HIP
	try {
		const int n = ggml_backend_cuda_get_device_count();
		for (int i = 0; i < n; ++i) {
			char buf[256] = {0};
			size_t free_b = 0, total_b = 0;
			ggml_backend_cuda_get_device_description(i, buf, sizeof(buf));
			ggml_backend_cuda_get_device_memory(i, &free_b, &total_b);

			BackendChoice c;
#if HAS_GGML_HIP
			c.kind = BackendKind::HIP;
#else
			c.kind = BackendKind::CUDA;
#endif
			c.device = i;
			c.desc = std::string(buf);
			c.vram_total = total_b;
			c.vram_free = free_b;
			out.push_back(std::move(c));
		}
	} catch (const std::exception &e) {
		blog(LOG_WARNING, "[whisper] CUDA/HIP device enumeration failed, skipping: %s", e.what());
	}
#endif

#if defined(__APPLE__)
	try {
		ggml_backend_t metal = ggml_backend_metal_init();
		if (metal) {
			ggml_backend_free(metal);
			BackendChoice c;
			c.kind = BackendKind::Metal;
			c.device = 0;
			c.desc = "Metal";
			out.push_back(std::move(c));
		}
	} catch (const std::exception &e) {
		blog(LOG_WARNING, "[whisper] Metal device enumeration failed, skipping: %s", e.what());
	}
#endif

#if HAS_GGML_VULKAN
	try {
		const int n = ggml_backend_vk_get_device_count();
		for (int i = 0; i < n; ++i) {
			char buf[256] = {0};
			size_t free_b = 0, total_b = 0;
			ggml_backend_vk_get_device_description(i, buf, sizeof(buf));
			ggml_backend_vk_get_device_memory(i, &free_b, &total_b);

			BackendChoice c;
			c.kind = BackendKind::Vulkan;
			c.device = i;
			c.desc = std::string(buf);
			c.vram_total = total_b;
			c.vram_free = free_b;
			out.push_back(std::move(c));
		}
	} catch (const std::exception &e) {
		blog(LOG_WARNING, "[whisper] Vulkan device enumeration failed, skipping: %s", e.what());
	}
#endif

	return out;
}

/*
    Best device of a given kind by reported total memory. Note that memory is used as
    proxy for "most powerful", which may not be true. Falls back to CPU if no GPU found
    
    devices:        list of available devices
    kind:           type of device requested
    
    returns: device
 
 */
static BackendChoice pickBestDeviceOfKind(const std::vector<BackendChoice> &devices, BackendKind kind)
{
	BackendChoice best;
	bool found = false;
	for (const auto &d : devices) {
		if (d.kind != kind)
			continue;
		if (!found || d.vram_total > best.vram_total) {
			best = d;
			found = true;
		}
	}
	if (!found)
		best.kind = BackendKind::CPU;
	return best;
}

/*
    Tries to choose a preferred backend, falls back to auto-select if not available
 */
static BackendChoice pickBestBackend(const std::string &preferred_id)
{
	const auto devices = enumerateGpuDevices();

	if (preferred_id == "cpu")
		return BackendChoice{};

	if (!preferred_id.empty() && preferred_id != "auto") {
		for (const auto &d : devices) {
			if (device_id(d) == preferred_id)
				return d;
		}
		blog(LOG_WARNING, "[whisper] Preferred GPU device '%s' not found - falling back to auto-select",
		     preferred_id.c_str());
	}

	// Auto priority: CUDA/HIP > Metal > Vulkan > CPU, mirroring the backend
	// build priority in CMakeLists.txt; best device by VRAM within a tier.
#if HAS_GGML_HIP
	{
		auto d = pickBestDeviceOfKind(devices, BackendKind::HIP);
		if (d.kind != BackendKind::CPU)
			return d;
	}
#elif HAS_GGML_CUDA
	{
		auto d = pickBestDeviceOfKind(devices, BackendKind::CUDA);
		if (d.kind != BackendKind::CPU)
			return d;
	}
#endif
#if defined(__APPLE__)
	{
		auto d = pickBestDeviceOfKind(devices, BackendKind::Metal);
		if (d.kind != BackendKind::CPU)
			return d;
	}
#endif
#if HAS_GGML_VULKAN
	{
		auto d = pickBestDeviceOfKind(devices, BackendKind::Vulkan);
		if (d.kind != BackendKind::CPU)
			return d;
	}
#endif

	return BackendChoice{}; // CPU fallback
}

/*
    List all available devices (except Apple Neural Engine)
 */
std::vector<GpuDeviceOption> WhisperWrapper::listAvailableDevices()
{
	static const auto backendLabel = [](BackendKind k) -> const char * {
		switch (k) {
		case BackendKind::CUDA:
			return "CUDA";
		case BackendKind::HIP:
			return "HIP";
		case BackendKind::Vulkan:
			return "Vulkan";
		case BackendKind::Metal:
			return "Metal";
		default:
			return "";
		}
	};

	std::vector<GpuDeviceOption> out;
	for (const auto &d : enumerateGpuDevices()) {
		std::ostringstream label;
		label << backendLabel(d.kind) << ": " << d.desc;
		if (d.vram_total > 0)
			label << " (" << (d.vram_total / (1024 * 1024)) << " MB)";
		out.push_back({device_id(d), label.str()});
	}
	return out;
}

/* shorthand for picking best auto backend */
std::string WhisperWrapper::resolveAutoDeviceId()
{
	return device_id(pickBestBackend("auto"));
}

/* get backend name from kind*/
static const char *backend_name(BackendKind k)
{
	switch (k) {
	case BackendKind::CUDA:
		return "CUDA";
	case BackendKind::HIP:
		return "HIP (ROCm)";
	case BackendKind::Vulkan:
		return "Vulkan";
	case BackendKind::Metal:
		return "Metal";
	default:
		return "CPU";
	}
}

/*
    Whisper.cpp log rerouting. Whisper logs a lot of stuff to stderr but we
    want it in the OBS log to enable troubleshooting. Whisper log is rerouted
    through here.
 */
static void whisperLogToObs(enum ggml_log_level level, const char *text, void * /*user*/)
{
	if (!text)
		return;
	// INFO and below is where the per-window VAD spam lives. GGML_LOG_LEVEL_CONT
	// is a continuation of whatever line preceded it, so it has to be dropped
	// too - keeping it would leak fragments of suppressed INFO lines.
	if (level != GGML_LOG_LEVEL_WARN && level != GGML_LOG_LEVEL_ERROR)
		return;

	// whisper's lines carry a trailing newline; blog adds its own.
	std::string msg(text);
	while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
		msg.pop_back();
	if (msg.empty())
		return;

	blog(level == GGML_LOG_LEVEL_ERROR ? LOG_ERROR : LOG_WARNING, "[whisper.cpp] %s", msg.c_str());
}

/* This installs the log handler to override stderr */
static void installWhisperLogHandlerOnce()
{
	static std::once_flag once;
	std::call_once(once, [] { whisper_log_set(whisperLogToObs, nullptr); });
}

WhisperWrapper::WhisperWrapper(const Config &cfg, Callback cb)
	: userConfig(cfg),
	  speechCallback(std::move(cb)),
	  liveConfig(cfg.live)
{
	if (userConfig.model_path.empty())
		throw std::runtime_error("model_path is empty");

	installWhisperLogHandlerOnce();

	// Model is shared so don't load directly
	whisperModel = WhisperModel::acquire(userConfig.model_path, userConfig.gpu_device_id);

	// Thread count comes from the model (WhisperModel::decodeThreads), not from here -
	// it is a property of the backend, which the model owns, and decodes against it are
	// serialised anyway.

	initVadOrFallback();

	// Last resort guard to ensure that unexopected thrown errors don't crash
	// OBS. Just kill the transcription and let OBS continue
	workerThread = std::thread([this] {
		try {
			this->run_loop();
		} catch (const std::exception &e) {
			blog(LOG_ERROR,
			     "[whisper] worker thread died on unexpected exception: %s - "
			     "transcription stopped (reload the filter to restart)",
			     e.what());
		} catch (...) {
			blog(LOG_ERROR, "[whisper] worker thread died on unexpected non-std exception - "
					"transcription stopped (reload the filter to restart)");
		}
		stopped_ = true;
	});
}

WhisperWrapper::~WhisperWrapper()
{
	stop();
}

/*
    Stop the model.
    -
    - wake the worker up as it may be wiating for its turn on the shared model
    - join the worker thread
 */
void WhisperWrapper::stop()
{
	// check if actually running, thread safely
	bool expected = false;
	if (!stopped_.compare_exchange_strong(expected, true))
		return;

	// secure the model mutex
	{
		std::lock_guard<std::mutex> lk(workerMutex);
		workerCV.notify_all();
	}

	// make sure the worker thread is actually awake to shut down and join it
	if (whisperModel)
		whisperModel->wake();
	if (workerThread.joinable())
		workerThread.join();

	// Release our allocation of the model. Last one out will actually delete the model
	// through reference counting
	whisperModel.reset();

	// After the worker join above, nothing else touches the VAD context.
	if (sileroVadContext) {
		whisper_vad_free(sileroVadContext);
		sileroVadContext = nullptr;
	}
}

/*
    Init the Silero Voice Activation Detection (VAD) or fall back to adapative
    RMS threshholding. Leaving the sileroVadContext == nullptr implies that the
    RMS fallback should be used.
 */
void WhisperWrapper::initVadOrFallback()
{
	if (userConfig.vad_model_path.empty()) {
		blog(LOG_INFO, "[whisper] No Silero VAD model configured - using the "
			       "RMS-energy VAD (set a model path to enable neural VAD)");
		return;
	}

	whisper_vad_context_params vparams = whisper_vad_default_context_params();

	// Single-threaded intentionally
	vparams.n_threads = 1;

	// Silero is a tiny LSTM run once per 32 ms run on CPU
	vparams.use_gpu = false;
	vparams.gpu_device = 0;

	// Defensive catch of unexpected thrown errors to keep OBS running.
	// this is just good manners
	try {
		sileroVadContext = whisper_vad_init_from_file_with_params(userConfig.vad_model_path.c_str(), vparams);
	} catch (const std::exception &e) {
		blog(LOG_ERROR, "[whisper] Silero VAD init threw: %s", e.what());
		sileroVadContext = nullptr;
	} catch (...) {
		blog(LOG_ERROR, "[whisper] Silero VAD init threw a non-std exception");
		sileroVadContext = nullptr;
	}

	if (!sileroVadContext) {
		blog(LOG_WARNING,
		     "[whisper] Failed to load Silero VAD model '%s' - "
		     "falling back to the RMS-energy VAD. Captioning still works; "
		     "background noise is just more likely to reach the decoder.",
		     userConfig.vad_model_path.c_str());
		return;
	}

	/*
        AFAIK Silero's detection window needs to be fixed at 512, which means some
        buffering is required as we cannot access Whisper's window
     */
	static constexpr int kSileroWindowSamples = 512;

	// try an empty sample buffer to probe if Silero is working
	const std::vector<float> probe(kSileroWindowSamples, 0.0f);
	bool probe_ok = false;
	try {
		probe_ok = whisper_vad_detect_speech(sileroVadContext, probe.data(), kSileroWindowSamples) &&
			   whisper_vad_n_probs(sileroVadContext) == 1;
	} catch (...) {
		probe_ok = false;
	}
	whisper_vad_reset_state(sileroVadContext); // discard the probe's LSTM state as mentioned above

	if (!probe_ok) {
		blog(LOG_WARNING,
		     "[whisper] Silero VAD model '%s' doesn't use the expected "
		     "%d-sample window (got %d probabilities for one window) - falling back "
		     "to the RMS-energy VAD rather than misreading its output",
		     userConfig.vad_model_path.c_str(), kSileroWindowSamples, whisper_vad_n_probs(sileroVadContext));
		whisper_vad_free(sileroVadContext);
		sileroVadContext = nullptr;
		return;
	}

	vadWindowSamples = kSileroWindowSamples;

	blog(LOG_INFO, "[whisper] Silero VAD loaded: %s (window=%d samples, %.0f ms, threshold=%.2f)",
	     userConfig.vad_model_path.c_str(), vadWindowSamples, 1000.0 * vadWindowSamples / WHISPER_SAMPLE_RATE,
	     (double)userConfig.live.vad_threshold);
}

// Get window samples for VAD - account for RMS fallback
int WhisperWrapper::getVadWindowSamples() const
{
	if (vadWindowSamples > 0)
		return vadWindowSamples;
	// Energy fallback: keep honouring the user's configurable frame size.
	std::lock_guard<std::mutex> lk(liveMutex);
	return std::max(1, (liveConfig.vad_frame_ms * WHISPER_SAMPLE_RATE) / 1000);
}

// This is the actual speech detection which has to be run repeatedly (every 32ms)
bool WhisperWrapper::vadDetectSpeech(const float *window, float threshold, float *probOut)
{
	if (probOut)
		*probOut = -1.0f;
	if (!sileroVadContext || !window)
		return false;

	// Do the speech detection with whisper_vad_detect_speech_no_reset which keeps the LSTM
	// state across calls, which is the what we need for streaming: each window is judged in
	// the context of the ones before it.
	bool ok = false;
	try {
		ok = whisper_vad_detect_speech_no_reset(sileroVadContext, window, getVadWindowSamples());
	} catch (const std::exception &e) {
		blog(LOG_ERROR, "[whisper] Silero VAD threw during detection: %s", e.what());
		return false;
	} catch (...) {
		blog(LOG_ERROR, "[whisper] Silero VAD threw a non-std exception during detection");
		return false;
	}
	if (!ok)
		return false;

	// One window in, so exactly one "is it speech" probability out.
	if (whisper_vad_n_probs(sileroVadContext) < 1)
		return false;
	const float p = whisper_vad_probs(sileroVadContext)[0];
	if (probOut)
		*probOut = p;
	return p > threshold;
}

/* null protected state reset */
void WhisperWrapper::vadResetState()
{
	if (sileroVadContext)
		whisper_vad_reset_state(sileroVadContext);
}

/* mutex guarded live config update*/
void WhisperWrapper::updateLiveConfig(const LiveConfig &live)
{
	std::lock_guard<std::mutex> lk(liveMutex);
	liveConfig = live;
}

/* mutex guarded live context geter*/
WhisperWrapper::LiveConfig WhisperWrapper::getLiveConfig() const
{
	std::lock_guard<std::mutex> lk(liveMutex);
	return liveConfig;
}

/*
    Push audio onto the queue for handling by the model. Do some rate
    management and reporting of overlooad
 */
void WhisperWrapper::pushAudio16k(const float *samples, int n_samples)
{
	if (!samples || n_samples <= 0)
		return;

	// Hard cap on buffered-but-unprocessed audio. If transcritpion can't keep up
	// drop the older frames so that at least the most recent speech gets through
	static constexpr size_t MAX_QUEUED_SAMPLES = 10u * WHISPER_SAMPLE_RATE; // 10 s

	size_t dropped = 0;
	{
		std::lock_guard<std::mutex> lk(workerMutex);
		for (int i = 0; i < n_samples; ++i)
			audioQueue.push_back(samples[i]);
		while (audioQueue.size() > MAX_QUEUED_SAMPLES) {
			audioQueue.pop_front();
			++dropped;
		}
	}
	workerCV.notify_one(); // don't forget to wake the worker again

	// Log on the transition into/out of the overloaded state, not per
	// drop - at 20ms audio callbacks a per-drop log would flood OBS's log.
	if (dropped > 0) {
		droppedSamples += dropped;
		if (!overloaded.exchange(true)) {
			blog(LOG_WARNING,
			     "[whisper] Transcription can't keep up with real time - "
			     "dropping oldest buffered audio (queue capped at %d s). "
			     "Consider a smaller model or a faster GPU device.",
			     (int)(MAX_QUEUED_SAMPLES / WHISPER_SAMPLE_RATE));
		}
	} else if (overloaded.exchange(false)) {
		blog(LOG_INFO,
		     "[whisper] Transcription caught up with real time again "
		     "(dropped %.1f s of audio while behind)",
		     (double)droppedSamples.load() / WHISPER_SAMPLE_RATE);
		droppedSamples = 0;
	}
}

/*
 
    We use a per rocess-global registry of loaded models, keyed on (path, device).
    Referenced by weak_ptr so an entry cannot keep a model. By reference conting
    the last WhisperWrapper to release the model also frees it.
 */
namespace {

/* associative array of models with mutex protecting access */
std::mutex modelsMutex;
std::map<std::string, std::weak_ptr<WhisperModel>> modelsMap;

// Create a model key using 0x1f to separae path and device. This symbol can't occur
// in a filesystem path or a device id, so is safe to use
std::string modelKey(const std::string &path, const std::string &device)
{
	return path + '\x1f' + device;
}

// Number of CPU threads to use whisper.cpp's own default. On a GPU backend n_threads sizes only the host-side
// pool for ops that were not dispatched to the device, so there is little to gain
// from raising it and real oversubscription to lose.
constexpr int kDefaultGpuDecodeThreads = 4;

} // namespace

/*
   acquire a model - either return exising or make a new one. Guarded with a mutex
 */
std::shared_ptr<WhisperModel> WhisperModel::acquire(const std::string &model_path, const std::string &gpu_device_id)
{
	const std::string key = modelKey(model_path, gpu_device_id);
	std::lock_guard<std::mutex> lock(modelsMutex);

	auto it = modelsMap.find(key);
	if (it != modelsMap.end()) {
		if (auto existing = it->second.lock()) {
			blog(LOG_INFO,
			     "[whisper] reusing the loaded model for %s on %s - "
			     "no second copy in memory",
			     model_path.c_str(), gpu_device_id.c_str());
			return existing;
		}
		modelsMap.erase(it); // last user released it; load again below
	}

	// Not make_shared: the constructor is private.
	std::shared_ptr<WhisperModel> model(new WhisperModel(model_path, gpu_device_id));
	modelsMap.emplace(key, model);
	return model;
}

/* remove the model on teardown */
void WhisperModel::invalidate(const std::string &model_path, const std::string &gpu_device_id)
{
	std::lock_guard<std::mutex> lock(modelsMutex);
	if (modelsMap.erase(modelKey(model_path, gpu_device_id)) > 0) {
		blog(LOG_INFO,
		     "[whisper] dropped the cached model for %s on %s - "
		     "the next source to (re)build will load it fresh",
		     model_path.c_str(), gpu_device_id.c_str());
	}
}

/* Instantiate the model - should be called via cache manager acquire */
WhisperModel::WhisperModel(const std::string &model_path, const std::string &gpu_device_id)
{
	backend_ = pickBestBackend(gpu_device_id);

	{
		std::ostringstream oss;
		oss << "[whisper] selected backend=" << backend_name(backend_.kind) << " dev=" << backend_.device
		    << " (" << backend_.desc << ")"
		    << " totalMB=" << (backend_.vram_total / (1024 * 1024));
		blog(LOG_INFO, "%s", oss.str().c_str());
	}

	whisper_context_params cparams = whisper_context_default_params();
	cparams.use_gpu = (backend_.kind != BackendKind::CPU);
	cparams.flash_attn = cparams.use_gpu;
	cparams.gpu_device = backend_.device;

	if (cparams.use_gpu) {
		// Usual exception protection
		try {
			context = whisper_init_from_file_with_params(model_path.c_str(), cparams);
		} catch (const std::exception &e) {
			blog(LOG_ERROR, "[whisper] GPU init threw, falling back to CPU: %s", e.what());
			context = nullptr;
		}
	} else {
		context = whisper_init_from_file_with_params(model_path.c_str(), cparams);
	}

	// Fall back to cpu if we failed to get a GPU when one was required
	if (!context && cparams.use_gpu) {
		// Hard fallback to CPU
		BackendChoice cpu;
		backend_ = cpu;

		cparams = whisper_context_default_params();
		cparams.use_gpu = false;
		cparams.flash_attn = false;
		cparams.gpu_device = 0;

		context = whisper_init_from_file_with_params(model_path.c_str(), cparams);
	}

	// Without a model no real point
	if (!context)
		throw std::runtime_error("Failed to init whisper model. Check model path / DLLs.");

	/*
        Number of CPU threads (preprocessing before it goes to GPU) is sized here at
        instantiation and hence once for all filters. There is no point adding more
        cores per filter as the requets are serialised to aovoid loading multpiple
        model instances.
     */
	if (const char *env = std::getenv("BABELSTREAMER_WHISPER_THREADS")) {
		decodeThreads_ = std::max(1, std::atoi(env));
		blog(LOG_INFO,
		     "[whisper] decode threads overridden to %d by "
		     "BABELSTREAMER_WHISPER_THREADS",
		     decodeThreads_);
	} else if (backend_.kind == BackendKind::CPU) {
		decodeThreads_ = (int)std::max(1u, std::thread::hardware_concurrency());
	} else {
		decodeThreads_ = kDefaultGpuDecodeThreads;
	}

	std::ostringstream oss;
	oss << "[whisper] backend=" << backend_name(backend_.kind) << " dev=" << backend_.device << " ("
	    << backend_.desc << ")"
	    << " totalMB=" << (backend_.vram_total / (1024 * 1024)) << " decodeThreads=" << decodeThreads_ << "\n";
	std::string msg = oss.str();
	blog(LOG_INFO, "%s", msg.c_str());
}

WhisperModel::Lease WhisperModel::lease(const std::atomic<bool> &cancel)
{
	std::unique_lock<std::mutex> lk(decodeMtx_);
	decodeConditionVariable.wait(lk, [&] { return !decodeBusy || cancel.load(); });

	// Note: the cancel can be modified duing the wait, so needs to be rechecked
	if (cancel.load())
		return Lease();

	decodeBusy = true;
	return Lease(this, context);
}

void WhisperModel::release()
{
	{
		std::lock_guard<std::mutex> lk(decodeMtx_);
		decodeBusy = false;
	}
	// notify_all, as you may need to wake a waiting thread to shut down
	decodeConditionVariable.notify_all();
}

WhisperModel::~WhisperModel()
{
	if (context) {
		whisper_free(context);
		context = nullptr;
	}
}

/*  
    Whisper reliably invents stock phrases when handed audio containing no
    speech - "Thank you for watching!", "and so on", "Please subscribe".
    This is an artefact of the training process.
 
    The approach is to inventory the stock phrases and discard on low
    recognition probability. Note that we will usually be using the Silero
    VAD which should perform better than the adaptive RMS, but the user still
    has the opportunity to set a low probability detectooin threshold, so we
    always need to account for this

    Whisper also has non-speech markers ("[BLANK_AUDIO]", "[Music]", "(applause)",
    "*coughs*", "♪♪") which also need to be discarded
 */

namespace {

std::string trimmedText(const std::string &s)
{
	const size_t a = s.find_first_not_of(" \t\r\n");
	if (a == std::string::npos)
		return {};
	const size_t b = s.find_last_not_of(" \t\r\n");
	return s.substr(a, b - a + 1);
}

bool containsNoCase(const std::string &haystack, const char *needle)
{
	const size_t n = std::strlen(needle);
	if (n == 0 || haystack.size() < n)
		return false;
	const auto eq = [](char a, char b) {
		return std::tolower((unsigned char)a) == std::tolower((unsigned char)b);
	};
	return std::search(haystack.begin(), haystack.end(), needle, needle + n, eq) != haystack.end();
}

/*
    Remove Whisper's non-speech markers. A marker embedded in text
    ("[Music] hi there") is deliberately left alone - dropping the whole
    segment would cost the speech, and a stray marker is merely cosmetic.
 */
bool isNonSpeechMarker(const std::string &t)
{
	if (t.size() >= 2 && ((t.front() == '[' && t.back() == ']') || (t.front() == '(' && t.back() == ')') ||
			      (t.front() == '*' && t.back() == '*'))) {
		return true;
	}

	// Segments made only of music notes: ♪ (U+266A) / ♫ (U+266B), compared
	// as raw UTF-8 bytes so this needs no locale or wide-char handling.
	static const char *const kNotes[] = {"\xE2\x99\xAA", "\xE2\x99\xAB"};
	std::string rest = t;
	for (const char *note : kNotes) {
		const size_t n = std::strlen(note);
		for (size_t p; (p = rest.find(note)) != std::string::npos;)
			rest.erase(p, n);
	}
	return rest != t && trimmedText(rest).empty();
}

/*
    boilerplate from the caption files whisper was trained on, which a live streamer
    will never actually say. Always safe to drop
*/
const char *const kAlwaysDrop[] = {
	"BLANK_AUDIO",          "amara.org",        "subtitles by",  "subtitled by",
	"subtitles created by", "transcription by", "captioning by",
};

/*
    Stock phrases whisper invents for non-speech audio. These are things that a
    streamer might say, so they are only dropped on low speech confidence
 */
const char *const kDropIfNoSpeech[] = {
	"thank you for watching",
	"thanks for watching",
	"thank you for listening",
	"please subscribe",
	"like and subscribe",
	"don't forget to subscribe",
	"see you next time",
	"see you in the next video",
	"thank you very much",
	"and so on",
	"and more",
	"bye bye",
};

/*
    drop a segment if its text matches a known hallucination phrase AND Whisper thinks
    there's even a moderate (>40%) chance the audio contained no speech
 */
constexpr float kFillerNoSpeechThold = 0.4f;

/*
    Generic backstop level for novel non-speech hallucinations. If whisper is very certain
    that it wasn't speech (hence the high threshhold value) we'll trust Whisper on this
 */
constexpr float kNoSpeechDropThold = 0.9f;

/*
    Check for hallucinated or non speech. Process is:
    1) Drop generic non-speech markers (i.e. [Laughter])
    2) Drop the stock filler phrases that a streamer would never use
    3) Drop the standard filler phrases if the no speech prob exceeds threshhold
    4) If we aren't forced trust as speech, then drop anything that Whisper thinks exceeds the thresshold
 */
bool isHallucinatedSegment(const std::string &raw, float no_speech_prob, bool trust_as_speech)
{
	const std::string t = trimmedText(raw);
	if (t.empty())
		return false; // nothing to emit either way

	if (isNonSpeechMarker(t))
		return true;

	for (const char *phrase : kAlwaysDrop)
		if (containsNoCase(t, phrase))
			return true;

	if (no_speech_prob > kFillerNoSpeechThold) {
		for (const char *phrase : kDropIfNoSpeech)
			if (containsNoCase(t, phrase))
				return true;
	}

	if (!trust_as_speech && no_speech_prob > kNoSpeechDropThold)
		return true;

	return false;
}

} // namespace

/*
    Transcribe a block of audio.
 */
std::string WhisperWrapper::transcribe(const std::vector<float> &audio, const LiveConfig &live, bool is_partial,
				       bool relax_no_speech_gate)
{
	whisper_full_params whisperParams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);

	whisperParams.print_progress = false;
	whisperParams.print_realtime = false;
	whisperParams.print_timestamps = false;
	whisperParams.print_special = false;

	whisperParams.translate = userConfig.translate;
	whisperParams.language = (userConfig.language == "auto") ? "auto" : userConfig.language.c_str();
	whisperParams.detect_language = (userConfig.language == "auto");
	whisperParams.n_threads = whisperModel->decodeThreads();

	/*
        If we need to relax the speech gate set it here, otherwise Whisper might discard
        the whole block. This is undesirable if we have split the speech on a timer
        boundary.
     */
	if (relax_no_speech_gate)
		whisperParams.no_speech_thold = 1.01f;

	/*
        Insert the static vocab hint. Points into the caller's live snapshot, never
        into liveConfig which may be modified on the fly. Needs to be carried over several
        (potentially split) utterances
     */
	whisperParams.initial_prompt = live.initial_prompt.empty() ? nullptr : live.initial_prompt.c_str();
	whisperParams.carry_initial_prompt = true;

	whisperParams.temperature = 0.0f;
	whisperParams.greedy.best_of = 1;
	whisperParams.single_segment = is_partial;

	// Turn this off because noise or splitting utterances can lead to Whisper repeating phrases
	whisperParams.no_context = true;

	/*
        After the lease is granted we have exclusive use of the model. If we don't get the
        lease this can only be because shutdown is in progress
     */
	auto lease = whisperModel->lease(stopped_);
	if (!lease)
		return {};
	whisper_context *ctx = lease.ctx();

	// Guard the unwanted exceptions as usual.
	try {
		if (whisper_full(ctx, whisperParams, audio.data(), (int)audio.size()) != 0) {
			// Also log the non-throwing failure path: a silent empty return here is indistinguishable from "no speech"
			blog(LOG_WARNING, "[whisper] whisper_full() failed (%s chunk, %.1f s of audio) - chunk dropped",
			     is_partial ? "partial" : "final", (double)audio.size() / WHISPER_SAMPLE_RATE);
			return {};
		}
	} catch (const std::exception &e) {
		blog(LOG_ERROR, "[whisper] whisper_full() threw (%s chunk): %s - chunk dropped",
		     is_partial ? "partial" : "final", e.what());
		return {};
	} catch (...) {
		blog(LOG_ERROR, "[whisper] whisper_full() threw a non-std exception (%s chunk) - chunk dropped",
		     is_partial ? "partial" : "final");
		return {};
	}

	/*  Assemble the text, dropping any segment that looks like a non-speech hallucination. Filtered per
        segment rather than over the concatenation, so one invented phrase can't discard real speech decoded alongside it
        and each segment is judged against its OWN no_speech_prob.
     */
	std::string out;
	const int nseg = whisper_full_n_segments(ctx);
	out.reserve(256);
	for (int i = 0; i < nseg; ++i) {
		const char *t = whisper_full_get_segment_text(ctx, i);
		if (!t)
			continue;

		const float nonSpeechProb = whisper_full_get_segment_no_speech_prob(ctx, i);
		if (isHallucinatedSegment(t, nonSpeechProb, relax_no_speech_gate)) {
			// Logged, not silent: this is the one place we throw away text
			// whisper actually produced, so it needs to be visible when
			// someone wonders where a caption went (and for tuning the
			// phrase lists against what real streams produce).
			blog(LOG_INFO,
			     "[whisper] dropped likely hallucination "
			     "(no_speech_prob=%.2f): %s",
			     nonSpeechProb, t);
			continue;
		}
		out += t;
	}
	return out;
}

/*
    Constants for RMS energy fallback when Silero not loaded. Effective threshold is
    max(kEnergyAbsFloor, noise_floor * kEnergyMult), and noise_floor is itself capped
    at kEnergyAbsFloor - so the floor sets the quiet room minimum while the multiplier
    adapts upward in noisier rooms.
 */
static constexpr float kEnergyAbsFloor = 0.0015f;
static constexpr float kEnergyMult = 3.0f;

/*
    Core run loop for the wrapper. Basically continually gets a chunk of audio.
    runs basic speech detection on it and either buffers or decoders it.
 */
void WhisperWrapper::run_loop()
{
	float noise_floor = 0.0f;
	bool in_speech = false;
	int speech_ms = 0;
	int silence_ms = 0;

	/*
        Build in a tolerance for dips below the speech threshold while we
        are accumulating enough audio to start speech. Every timing in this
        function was originally tuned  against the RMS-energy VAD's frame-to-frame jitter.
     */
	const int speech_hangover_ms = 40; // ~2 frames at the default 20ms
	int speech_gap_ms = 0;

	/*
        Preroll window kept while !in_speech, prepended to the utterance once
        speech is confirmed.
     */
	const int preroll_ms = 300;
	const int preroll_samples = (preroll_ms * WHISPER_SAMPLE_RATE) / 1000;
	std::deque<float> preroll;

	/*
        Carried into the next chunk only when a split to a lengthCap. fires (see
        That cut point is arbitrary, not a natural pause, so a word straddling it
        would otherwise have its audio split across two independently-transcribed
        chunks with no context on either side.
     */
	const int lengthcap_overlap_ms = 300;
	const int lengthcap_overlap_samples = (lengthcap_overlap_ms * WHISPER_SAMPLE_RATE) / 1000;

	std::vector<float> utterance;
	utterance.reserve(WHISPER_SAMPLE_RATE * 10);

	auto last_partial = std::chrono::steady_clock::now();
	std::string last_partial_text;

	// Elapsed samples of 16 kHz audio consumed since this run_loop started
	int64_t samplesConsumed = 0;
	int64_t utteranceStartSamples = 0;

	while (!stopped_) {
		/*
            Re-read the hot-swappable settings every frame, so an updateLiveConfig()
            from the UI thread takes effect within one  VAD frame instead of needing a
            model reload.
         */
		const LiveConfig live = getLiveConfig();

		/*
            With Silero active the frame IS its analysis window (512 samples/32 ms)
            so vad_frame_ms therefore has no effect in neural mode, but still governs
            the RMS energy fallback. Clamped because a zero frame size would loop
         */
		const int frame_samples = std::max(1, getVadWindowSamples());

		const int frame_ms = std::max(1, (frame_samples * 1000) / WHISPER_SAMPLE_RATE);

		const int max_utterance_samples = (live.max_utterance_ms > 0)
							  ? (live.max_utterance_ms * WHISPER_SAMPLE_RATE) / 1000
							  : 0; // 0 = unlimited

		std::vector<float> frame;
		frame.reserve(frame_samples);

		{
			std::unique_lock<std::mutex> lk(workerMutex);
			workerCV.wait(lk, [&] { return stopped_ || (int)audioQueue.size() >= frame_samples; });
			if (stopped_)
				break;

			for (int i = 0; i < frame_samples; ++i) {
				frame.push_back(audioQueue.front());
				audioQueue.pop_front();
			}
		}
		samplesConsumed += frame_samples;

		// This section does the speech/nonspeech determintaion
		bool is_speech = false;
		float energy_rms = 0.0f; // only meaningful on the fallback path

		if (usingNeuralVad()) {
			// This is the preferred neural VAD path
			is_speech = vadDetectSpeech(frame.data(), live.vad_threshold, nullptr);
		} else {
			// Fallback heuristic - live only when no Silero model loaded.
			energy_rms = rms(frame.data(), (int)frame.size());
			is_speech = (energy_rms > std::max(kEnergyAbsFloor, noise_floor * kEnergyMult));
		}

		if (!in_speech) {
			/*
                For the fallback speech detection - slowly adapt to the ambient sound.
                Only run when not in speech to avoid classification errors
             */
			if (!usingNeuralVad() && !is_speech) {
				const float noise_floor_alpha = 0.05f;
				noise_floor += noise_floor_alpha * (energy_rms - noise_floor);
				noise_floor = std::min(noise_floor, kEnergyAbsFloor);
			}

			// update the preroll buffer
			for (float s : frame)
				preroll.push_back(s);
			while ((int)preroll.size() > preroll_samples)
				preroll.pop_front();

			// This is dealing with speech when we are not yet in a "we have speech" mode
			if (is_speech) {
				// If we have enough speech to start treating it as speech, mark and add the buffer
				speech_ms += frame_ms;
				speech_gap_ms = 0;
				if (speech_ms >= live.vad_min_speech_ms) {
					in_speech = true;
					silence_ms = 0;

					utterance.clear();
					utterance.insert(utterance.end(), preroll.begin(), preroll.end());
					preroll.clear();
					utterance.insert(utterance.end(), frame.begin(), frame.end());
					last_partial_text.clear();
					last_partial = std::chrono::steady_clock::now();

					// utterance now holds exactly [preroll][this frame], and
					// samplesConsumed already counts through this frame (see
					// above) - so the position of utterance's first sample on
					// the audio timeline is just the difference.
					utteranceStartSamples = samplesConsumed - (int64_t)utterance.size();
				}
			} else if (speech_ms > 0) {
				// Already mid-run: tolerate a brief dip instead of resetting immediately (see speech_hangover_ms above).
				speech_gap_ms += frame_ms;
				if (speech_gap_ms > speech_hangover_ms) {
					speech_ms = 0;
					speech_gap_ms = 0;
				}
			}
		} else {
			/*
                this is the bit when we already in speech. Now we have to decide if we have
                enough speech to send to the model to be recognised
             */
			silence_ms = is_speech ? 0 : silence_ms + frame_ms;
			utterance.insert(utterance.end(), frame.begin(), frame.end());

			// partial update
			if (live.partial_every_ms > 0) {
				const auto now = std::chrono::steady_clock::now();
				const auto dt =
					std::chrono::duration_cast<std::chrono::milliseconds>(now - last_partial)
						.count();
				if (dt >= live.partial_every_ms) {
					last_partial = now;

					const int win_samples = (live.partial_window_ms * WHISPER_SAMPLE_RATE) / 1000;
					const int start = std::max(0, (int)utterance.size() - win_samples);
					std::vector<float> window(utterance.begin() + start, utterance.end());

					std::string txt = transcribe(window, live, true);
					if (!txt.empty() && txt != last_partial_text) {
						last_partial_text = txt;
						speechCallback(txt, false, 0, 0); // timing unused on a partial
					}
				}
			}

			/*
                end of utterance: either natural trailing silence, or a forced split because
                the speaker has been going non-stop and the buffer hit max_utterance_samples.
             */
			const bool naturalEnd = silence_ms >= live.vad_end_silence_ms;
			const bool lengthCap = max_utterance_samples > 0 &&
					       (int)utterance.size() >= max_utterance_samples;

			if (naturalEnd || lengthCap) {
				// Relax whisper's no-speech gate only on a forced split as above
				std::string final_txt = transcribe(utterance, live, false, !naturalEnd);
				if (!final_txt.empty()) {
					/*
                        naturalEnd's utterance buffer includes the trailing silence that
                        confirmed it (vad_end_silence_ms worth). Subtract it back off so end_ms
                        marks where speech actually stopped, not where the pause was long enough
                        to notice. lengthCap has no such trailing silence (still mid-speech when the cap hit),
                        so its end is simply "now".
                    */
					const int64_t silenceSamples =
						naturalEnd ? ((int64_t)silence_ms * WHISPER_SAMPLE_RATE) / 1000 : 0;
					const int64_t endSamples =
						std::max(utteranceStartSamples, samplesConsumed - silenceSamples);
					const int64_t startMs = (utteranceStartSamples * 1000) / WHISPER_SAMPLE_RATE;
					const int64_t endMs = (endSamples * 1000) / WHISPER_SAMPLE_RATE;
					speechCallback(final_txt, true, startMs, endMs);
				}

				silence_ms = 0;
				last_partial_text.clear();
				last_partial = std::chrono::steady_clock::now();

				if (naturalEnd) {
					utterance.clear();
					in_speech = false;
					speech_ms = 0;
					speech_gap_ms = 0;
					preroll.clear();

					/*
                        Clear Silero's LSTM state now that this utterance is genuinely over.
                        If you don't then the last state will bias the next state which can lead
                        to an unwanted drift.- what whisper.h's detect_speech_no_reset
                    
                        Not done on the lengthCap because we're still inside a truncated utterance
                     */
					vadResetState();
				} else {
					/*
                        Add a trailing overlap as we are actually mid utterance and don't want
                        to lose to much context for the next part
                     */
					const int keep = std::min((int)utterance.size(), lengthcap_overlap_samples);
					std::vector<float> overlap(utterance.end() - keep, utterance.end());
					utterance = std::move(overlap);
				}
			}
		}
	}
}
