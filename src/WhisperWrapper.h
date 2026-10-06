// SPDX-FileCopyrightText: 2026 Kamayu B.V. <https://www.kamayu.nl>
// SPDX-License-Identifier: GPL-2.0-or-later

//=======================================================================
//
//  WhisperWrapper.h
//
//  Created: 20 sep 2026
//
//  Author: N.H. Elroy with support from Claude Code (Sonnet)
//
//  Abstract: Wrapper for Whisper.cpp speech recognition, using Silero neural VAD
//            for speech detection
//
// [Doxygen part]
//
///    Core of the plugin. Manages the Whisper model for speech recognition and the Silero model for
///    speech transcription. The approprate Whisper model is fed audio fragments as either partial or
///    complete utterances and returnes partial or complete transciptions which are injected into the
///    main loop via a callback. Silero VAD is used for speech detection unless loading fails, in which
///    case a fallback adaptive RMS threshhold is used.
///
///    This frontend deals with the user parameters, chiefly:
///     - preferential recognition of gaming, specific game and user vocab
///     - speech detetion and cutoff parameters
///     - threshholds
///
///    Note that the speech recognition threshholding is vital to transcription quality as Whisper will just
///    emit stock phrases like "Thank you for watching" if there is a low probability transcription.
///
///    The whisper model is shared over all instances of the plugin running in order to conserve GPU resources.
///    This means that access is gated and queued between the filters which also means that you don't get
///    true overlapping speech.
//
//=======================================================================

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <whisper.h>



/**
    Backend types
 */
enum class BackendKind { CPU, CUDA, HIP, Vulkan, Metal };

/**
    Backend choice info
 */
struct BackendChoice {
	BackendKind kind = BackendKind::CPU; /*! Type of backend */
	int device = 0;                      /*! Device numeber*/
	std::string desc;                    /*! Text device descriptor */
	size_t vram_total = 0;               /*! Total RAM */
	size_t vram_free = 0;                /*! Available RAM*/
};

/**
    One selectable entry for a GPU-choice UI.
 */
struct GpuDeviceOption {
	std::string id;    /*! stable identifier to persist in settings, e.g. "vulkan:0", "cuda:1" */
	std::string label; /*! human-readable, e.g. "Vulkan: NVIDIA GeForce RTX 5060 (8188 MB)" */
};

/**
 * The loaded whisper model — the expensive half of transcription (1GB or more)  Note that the design
 * supports multiple filters being loaded for multiple audio inputs. To make this workable
 * the filters have a single instance of the model that they must share. This implies that
 * if there is overlapping speech from several sources they must be queued for  recogniton.
 *
 * A single filter instance requires a Silero VAD context (a couple of MB) and a worker
 * thread
 *
 * The Model object is reference counted and keyed on (model path, device): sources asking for the same
 * pair share an instance, different pairs each get their own. Note that the option of using multiple models
 * is not actually available, but is included for later expansio
 *
 */
class WhisperModel {
public:
	/** The already-loaded model for this (path, device), or a freshly loaded  one. Throws if it cannot be loaded at all. */
	static std::shared_ptr<WhisperModel> acquire(const std::string &model_path, const std::string &gpu_device_id);
	~WhisperModel();

	/**
        Forget the cached instance for this key to force loading of a new one. Because the object is ref
        counted this is safe, as the old object will persist until the last user drops it. This is necessary
        for when e.g. the Core ML decoder is modified or added
     
        @param model_path       path to model
        @param gpu_device_id   device id
     */
	static void invalidate(const std::string &model_path, const std::string &gpu_device_id);

	WhisperModel(const WhisperModel &) = delete;
	WhisperModel &operator=(const WhisperModel &) = delete;

	/** The backend actually in use, which may be CPU even when a GPU was asked
        for (init falls back).
     */
	BackendKind kind() const { return backend_.kind; }

	/**
        Threads to pass as whisper_full_params::n_threads for a decode on this model.

        This belongs to the model, not to a source, for the same reason the processing device
        does. Decodes are serialised by lease(), so every source's decode runs against this one
        context with the whole machine to itself — and a per-source count meant the effective
        value depended on whichever source happened to hold the lease, which is invisible and
        non-deterministic once more than one speaker is captioned.

        Note this is a CPU thread count even on a GPU backend: ggml dispatches the tensor work
        to the device, and n_threads only sizes the host-side pool for what is left. It is
        therefore not something a streamer can reason about, which is why it is no longer a
        setting. BABELSTREAMER_WHISPER_THREADS overrides it for the rare install that needs to.
     */
	int decodeThreads() const { return decodeThreads_; }

	/**
        Serialise the recognition from different audio sources by gaining exclusive access (lease) to
        the model. Lease lasts until the decoded speech has been read out of the model context.
        Wait for lease is cancellable to allow tear down on exit or reset, or in cases where the
        processing cannot keep up with the speech
     */
	class Lease {
	public:
		/** Always release context on destruction*/
		~Lease()
		{
			if (owner)
				owner->release();
		}

		/**
            Make this non-copyable by declaring an explicit copy constructor  which nulls out the data,
         */
		Lease(Lease &&other) noexcept : owner(other.owner), context(other.context)
		{
			other.owner = nullptr;
			other.context = nullptr;
		}

		/** non copyable by constructor */
		Lease(const Lease &) = delete;

		/** non copyable by equality operator*/
		Lease &operator=(const Lease &) = delete;

		/** getter for whisper context */
		whisper_context *ctx() const { return context; }

		/** explicity test on defined conntext */
		explicit operator bool() const { return context != nullptr; }

	private:
		friend class WhisperModel;
		Lease() = default;
		Lease(WhisperModel *o, whisper_context *c) : owner(o), context(c) {}
		WhisperModel *owner = nullptr;
		whisper_context *context = nullptr;
	}; // end of Lease


	/**
        Wait for a lease (exclusive access) to a model context, with potential cancel during wait
     
        @param cancel atomic cancel flag
     */
	Lease lease(const std::atomic<bool> &cancel);

	/**
        Re-evaluates every waiter's cancel flag. Called by an instance that stopping, before it joins its worker.
     */
	void wake() { decodeConditionVariable.notify_all(); }

private:
	friend class Lease;
	WhisperModel(const std::string &model_path, const std::string &gpu_device_id);
	void release();

	BackendChoice backend_;
	whisper_context *context = nullptr;

	/*
     Sized in the constructor, after any fallback to CPU has resolved, so it always
     matches the backend the model actually landed on rather than the one requested.
     */
	int decodeThreads_ = 1;

	std::mutex decodeMtx_;
	std::condition_variable decodeConditionVariable;
	bool decodeBusy = false;
};

/**
 * Real-time speech transcription using whisper.cpp with Silero neural VAD
 * (RMS-energy fallback when no Silero model is configured).
 *
 * Feed 16 kHz mono float32 audio via pushAudio16k().
 * The supplied callback fires with (text, is_final, start_ms, end_ms) on the
 * worker thread.
 *
 * Thread-safe: pushAudio16k() may be called from any thread.
 */
class WhisperWrapper {
public:
	/** Callback definition for handling speech fragment received (utterance).
        The timings locate the utterance on this WhisperWrapper instance's
        own audio timeline — elapsed milliseconds of 16 kHz audio rather than
        less accurate system clock. Resets to 0 whenever a new WhisperWrapper is built.
      
        Only meaningful when is_final is true — both are 0 on a partial
        result, since partials are never persisted
    
        @param text recognised text
     
        @param is_final utterance is final i.e. complete text fragment
     
        @param start_ms start of utterance in audio timeline.
     
        @param end_ms end of utterance in audio timeline.
     */
	using Callback = std::function<void(const std::string &text, bool is_final, int64_t start_ms, int64_t end_ms)>;

	/*
        Structure for maintaining configurable settings for how the speech is processed and recognised.
        These settings can be updated live.
 
        The free-text vocabulary/name hints (e.g. streamer name, game titles, is fed to whisper to bias
        speech recognition to a specific vocab.
     */
	struct LiveConfig
    {
		int vad_frame_ms = 20;        /*! VAD analysis frame size */
		int vad_min_speech_ms = 120;  /*! minimum speech run to start utterance */
		int vad_end_silence_ms = 260; /*! trailing silence to end utterance */
		int max_utterance_ms = 30000; /*! max speech length before capping and emitting utterance*/
		int partial_every_ms = 350;   /*! frequencey of generating parital recongnition (0=off) */
		int partial_window_ms = 2500; /*! audio window for partial decoding */
		float vad_threshold = 0.5f;   /*! probabilty for Silero to accept as speech */
		std::string initial_prompt;   /*! Free text vocabulary biasing (see above) */
	};

	/**
        Structure for maintaining all configurable settings including those that cannot be updated live. Update of these
        settings requires a new WhisperWrapper object (LiveConfig can be updated separately without reload)
     */
	struct Config
    {
		std::string model_path; /*! Path to Whisper model local*/
		std::string gpu_device_id =
			"auto"; /*! Device to run model - available devices returned by listAvailableDevices */
		std::string language = "en"; /*! BCP-47 or "auto" */
		bool translate = false;      /*! When true, enable translation */
		std::string vad_model_path;  /*! Silero ggml model for speech detection */

		LiveConfig live; /*! current live config */
	};

	/**
        Instantiate the model.
     
        @param cfg     configuration
        @param cb      callback to server recognised text utterances
     */
	WhisperWrapper(const Config &cfg, Callback cb);

	/**
        Destructor just calls stop()
     */
	~WhisperWrapper();

	/**
        Stop the model and do approprite tear down
    */
	void stop();

	/**
        Queue a sample buffer for recognition
     
        @param samples         sample buffer
        @param n_samples   number of samples in buffer
     */
	void pushAudio16k(const float *samples, int n_samples);

	/**
        Hot-swap the live settings on a running instance — VAD timings and custom
        vocabulary, applied within a single VAD frame with no model reload. Safe to call from any thread
        (typically the UI thread, from an OBS settings update).
     
        @param live new live config
     */
	void updateLiveConfig(const LiveConfig &live);

	/**
        Enumerate all available GPU devices ( so onot Apple Neural Engine or CPU)
     
        @returns list of available GPUs
     */
	static std::vector<GpuDeviceOption> listAvailableDevices();

	/**
        Get the id of the best available device for the model based upon what "auto" would pick.
        Note that this will resolve GPUs or CPU but NOT Apple Neural Engine which has a different path
        In general this will be the GPU with highest RAM, or CPU if no GPU available.
     
        @returns processing device name
     */
	static std::string resolveAutoDeviceId();

	/**
        Check if Silero is being used for speech detecttion
     
        @returns true if Silero used, otherwise assume RMS fallback
     */
	bool usingNeuralVad() const { return sileroVadContext != nullptr; }

private:
	/*
        Initiate the VAD. Try Silero and if this fails fall back to RMS
     */
	void initVadOrFallback();

	/*
        Detect speech. Speech/not-speech test for exactly one Silero window
        (vadWindowSamples()). Returns the raw probability via "probOut" for
        logging purposes. Must be called on consecutive, non-overlapping windows —
        Silero is an LSTM network, so its state carries across calls (which is what
        makes it far steadier than a per-frame energy threshold, and why
        vadResetState() matters at utterance boundaries).
     
        window      a window's worth of samples
        threshold   probability threshold for speech detection
        probOut     actual probability of speech
     
        returns     if true then probably speech
     */
	bool vadDetectSpeech(const float *window, float threshold, float *probOut);

	/* reset VAD state (obviously) */
	void vadResetState();

	/*
        Get the number of VAD window samples. Silero has a fixed window, but if we
        are falling back to the RMS detection then this is derived from userConfig.live.vad_frame_ms
     */
	int getVadWindowSamples() const;

	/*
        Transcribe and audio fragment.
     
        audio:                  audio fragment
        live                    current live config. Note that live is not protected to avoid races
        is_partial              sample is a partial
        relax_no_speech_gate    cut off regardless of end of speeh detection
     */
	std::string transcribe(const std::vector<float> &audio, const LiveConfig &live, bool is_partial,
			       bool relax_no_speech_gate = false);
	void run_loop();

	/*
        Getter for current config in protected thread-safe call

        returns current config
     */
	LiveConfig getLiveConfig() const;

	/*
        Calculate current rms. Used mainly as fallback for Silero not being available
                
        x : array of samples
        n : number of samples
        
        returns RMS value
     */
	static float rms(const float *x, int n)
	{
		double s = 0.0;
		for (int i = 0; i < n; ++i)
			s += double(x[i]) * double(x[i]);
		return (n > 0) ? float(std::sqrt(s / n)) : 0.0f;
	}

	Config userConfig;       /* construction-time only; userConfig.live is just the seed for liveConfig */
	Callback speechCallback; /* callback for recognised speech */

	mutable std::mutex liveMutex; /* mutex for accessing liveConfig */
	LiveConfig liveConfig; /* current live configuration,  read via liveConfig(), replaced via updateLiveConfig() */

	std::shared_ptr<WhisperModel>
		whisperModel; /* shared Whisper speech model for all instances of filter. Reference counter */

	/** Silero VAD for speech thresholding */
	whisper_vad_context *sileroVadContext =
		nullptr;          /* context for Silero VAD, null if none or load failed. Only used by worker thread*/
	int vadWindowSamples = 0; /* from the model; 0 until one loads */

	/** worker thread */
	std::atomic<bool> stopped_{false}; /* worker thread status*/
	std::thread workerThread;          /* worker thread */
	mutable std::mutex workerMutex;    /* mutex for access to worker thread */
	std::condition_variable workerCV;  /* condition for variable for notification of worker thread*/

	/** audio queue */
	std::deque<float> audioQueue;          /* queue of audio fragments to be recognised*/
	std::atomic<bool> overloaded{false};   /* have we overloaded the recogniser (@see pushAudio16k */
	std::atomic<size_t> droppedSamples{0}; /* samples dropped due to overload*/
};
