# BabelStreamer-filter — maintainer/developer guide

Technical documentation for building, packaging, and maintaining
this plugin. For end-user install/usage instructions, see the
[OBS Plugin Manual](https://babelstreamer.com/manual.html). There is 
also a READMEFIRST bundled with the download which nobody ever reads but I still maintain
because I have an unreasonable level of faith in humanity.

The code is internally documented with Doxygen markup and the documentation
can be generated with Doxygen.

## What this is

An OBS Studio **audio filter** (`obs_source_info`, `OBS_SOURCE_TYPE_FILTER`) 
that acts as a frontend to the (subscription based) BabelStreamer
translation service. The plugin converts speech to texts and servese JSON encoded captions 
to a remote translation
and distribution server run by BabelStreamer, which then distributed to viewers in 
multiple simultaneous languages with zero or near zero latency.

The plugin can also function as a standalone text-to-speech converter which can generate
captions that can be linked to a text field chosen by the user.

Speech is transcribed locally using the whisper model with
[whisper.cpp](https://github.com/ggml-org/whisper.cpp).

The plugin is configured for Windows and MacOS. GPUs are accessed via VULKAN and on
Apple Silicon the Apple Neural Engine may be used

The following configurations are supported:

| OS | CPU | Execution Platform |
|--|--|--|
| Windows | Intel | VULKAN, CPU |
| MacOS | Intel | Metal, CPU |
| MacOS | Apple Silicon | Apple Neural Engine, Metal ,CPU |



## Project layout

```
src/
  BrandedPopup.{hpp,cpp}			Generate popup dialogues in BabelStreamer house style
  DeviceLinkDialogue. {hpp,cpp} 		Dialogue for linking the current devie to a BabelStreamer account
  FilterOrigin.{hpp,cpp}			Utilities for resolving BabelStreamer server name
  FilterProperties.{hpp,cpp} 		Handle the OBS properties page for the filter
  GameVocab.{hpp,cpp}		 	Download, cache and restore game specific vocab lists
  ModelDownloadDialog.{hpp,cpp} 		Download Whisper models dialogues
  ServerLink.{hpp,cpp}		 	Manage link with BabelStreamer server
  SharedSettings.{hpp,cpp}   		Settings shared by all instances of the filter
  TranslatorPlugin.{cpp}	 		obs_module_load()/unload(), registers the filter.
  UpdateChecker.{hpp,cpp}			Check for updates on BabelStreamer site
  WebsocketClient.{hpp,cpp} 		Minimal WebSocket client used to talk to server
  WhisperEngine.{hpp,cpp}			Drives Whisper.cpp and handles config
  WhisperFilter.{hpp,cpp}   		Filter implementation: OBS callbacks, settings, identity etc.
  WhisperWrapper.{h,cpp}     		VAD + whisper.cpp transcription worker thread.
  plugin-support.{h,c.in}    		OBS plugin-template boilerplate (log prefix etc).
cmake/                        
					Build config (see Building below).
```

## Building

**Requirements (all platforms):** a build of
[whisper.cpp](https://github.com/ggml-org/whisper.cpp) — CMake looks for it
via `WHISPER_ROOT` (each preset below sets a default).

### macOS builds

```sh
./setup_macos.sh
```

Configures with the `macos` preset (`PLUGIN_STANDALONE_XCODE=ON`) and opens
`build_macos/babelstreamer-filter.xcodeproj`. This preset deliberately has **no
install/package targets** — it's meant for iterating and for a manual Xcode
Archive when you just want to test a signed build locally. Requires Xcode
command-line tools + CMake 3.28+.

A notarised binary with installer is maintained and downloadable on the 
[BabelStreamer](https://www.babelstreamer.com/obs-plugin-download.html) website.


### Windows builds

```bat
setup_windows.bat [path\to\whisper.cpp]
```

Configures with the `windows-x64` preset and opens
`build_x64\babelstreamer-filter.sln` (Visual Studio 2022, "Desktop development
with C++"; CMake 3.28+). `WHISPER_ROOT` must contain `include\whisper.h` and
a built `whisper.lib`

***Note although the build has configurations for HIP and CUDA, VULKAN is the only
tested configuration and the others are not fully supported here due to licensing 
considerations. This will be addressed in a later version ***

A signed binary with installer is maintained and downloadable on the [BabelStreamer](https://www.babelstreamer.com/obs-plugin-download.html) website.



### CMake presets reference

Defined in [`CMakePresets.json`](CMakePresets.json):

| Preset | Purpose |
|---|---|
| `macos` | Standalone Xcode project, no install/package targets. Local dev + manual Archive/notarize. |
| `macos-ci` | Same as `macos` but with install/package targets enabled — used by the maintainer's (private) release-signing tooling and CI. |
| `windows-x64` | Visual Studio 2022 solution for local dev. |
| `windows-ci-x64` | Same, with warnings-as-errors, for CI. |


## Voice activity detection (Silero)

Speech/non-speech detection uses **Silero VAD** — a small LSTM (864 KB as
ggml) with fallback to adaptive RMS levels. The model is quite small and
ships with the plugin if you download the binary. The Silero VAD can be downloaded at 
https://huggingface.co/ggml-org/whisper-vad

**Upgrading the model** (e.g. to silero-v6): drop the new file in `data/`,
delete the old one, and update `VAD_MODEL_FILENAME` in
[`src/whisper-filter.hpp`](src/whisper-filter.hpp). Init verifies the 512-sample
window and falls back if a future model differs (see below).


### Implementation notes
- **Standalone usage vs. BabelStreamer frontend**
	The plugin is intended as the OBS front end to the babelstreamer live translation service
	(www.babelstreamer.com) but can also be used as a standalone speech-to-text plugin 
	using the Whisper model. No changes to the code are needed, but the user needs to 
	link the filter to an OBS text field to display. For debugging pourposes the recognised
	speech is logged to the OBS log.
- **Multiple Filter Management**
   The plugin is designed to allow multiple audio sources to be using it at the same 
   time. This requires quite a lot of code to ensure that shared resources are dealt
   with correctly. In particular:
   
   - All filters share a single Whisper model to avoid overloading GPU memory
   - There is only one WebSocket link to the server shared across all filters
   - User interaction has to be managed as if there is only one filter.
   
  This also has consequences for the managing of Whisper context as different
  audio streams will break context and may even be in different languages
- **Latency considerations** One aim of the plugin is to provide a low latency 
  translation in the optional translation stream. BabelStreamer achieves effective
  zero latency by managing the streaming "inflight" delay to the translation delay.
  This does require the initial caption to be generated reasonably close to the 
  actual speech. To facilitate this there is a configurable forced cutoff

- **Default Speech Settings** a number of default settings (e.g. speech floor) differ from what
is recommended or standard. These settings have been arrived at by lots of experimentation
in a typical streamer setting.

- **whisper's log is routed to the OBS log** (`whisperLogToObs`).This helps in managing
the volume of Whisper logging, but also enables more debug info to be retained in the 
OBS logs.

- **Default Device Choice**
The default device choice is 
	- Apple Neural Engine on Apple Silicon
	- Metal on Intel macs
	- GPU if available on Windows. If several GPU's are available then the one with the
	  most memory is chosen as a proxy for "most powerful". This is not always the best
	  choice but we haven't yet implemented a GPU list in power order.
	- Fallback to CPU if no GPU available. At time of writing this will only work with
	  the smalles models.
	
- **Language choice** there is a dropdown to select speaker language. This has no effect 
in the actual plugin and is used to drive the translation process on the backend.

- **Model Choice** the plugin doesn't ship with a model because this is 1Gb plus. There is
a standard download dialogue which preselects either an English only model or a multilingual
model. Both models are chosen to be about 1Gb - this is not the largest model, but tests
show they work well. The larger model (or indeed any other model) can be optionally
selected. Using Apple Neural Engine there are additional download prompts to download
the necessary files, which are automatically selected based on the conventions used in the
Hugging Face repo.

- **Neural Engine Symlink**
the plugin assumes that Apple Silicon has Neural Engine and downloads the additional decoder
after checking with the user. Because Whisper.cpp doesn't have an actual Neural Engine setting.
If Whisper is linked with HAS_WHISPER_COREML and a compiled model exists in the derived
path "<model>-encoder.mlmodelc" next to the .bin, Whisper will use the neural engine. Because
the user is free to download the ANE decoder anywhere, the plugin creates a symlink in the 
appropriate directory rather than copying 1 Gb+ file. 


### Compromise decisions, not mistakes

- **Per-call graph rebuild is not worth fixing.**
  `whisper_vad_detect_speech_no_reset()` rebuilds its whole ggml graph every
  call and amortizes it over however many chunks it's handed; we hand it one.
  The tradeoff is worth to handle the context issues discussed above
- **Timings are tuned for RMS speech detecion**  `speech_hangover_ms` (40),
  `preroll_ms` (300), min-speech (120), end-silence (260) were tuned against
  the energy VAD's performance. Silero is much steadier, so several
  are probably more generous than needed. Left at proven values on purpose
  to allow fallback to work reasonably.

## Source language choice with multilingual models

the source language is limited to the Whisper model languages, currently
99 at last count and these will work for _transcription_. Whisper autodectects
language and the choice in the UI has no effect. However when using the
plugin as a front end for the babelstreamer translation service this has two
major backend consequences:
- The backend cannot auto detect the language and needs to be told because their is a large
amount of manipulation needed to improve translation quality and manage niche vocab.
This involves language specific grammar manipualation
- We test all language pair translations and compensate for issues in the backend. This
is why the number of reliably translatable source languages is lower than the available
list.


## License

GPL-2.0-or-later — see [LICENSE](LICENSE). "Or later" matters here: this
plugin links Qt 6 (LGPL-3.0) and, on the `wss://` path, mbedTLS
(Apache-2.0/GPL-2.0-or-later dual license); neither is compatible with a
GPL-2.0-*only* combined work, and "or later" is what lets the combination
resolve under GPL-3.0. See [THIRD-PARTY-LICENSES.md](THIRD-PARTY-LICENSES.md)
for the full breakdown of what's bundled, statically linked, and
dynamically linked against the host OBS Studio installation.

License has been referenced in every source file for completeness.


## OBS Forums submission compliance

See [COMPLIANCE.md](COMPLIANCE.md) for how this plugin meets the OBS
Forums resource submission policy, including licensing, release history,
the download link, and disclosure of AI use in its development (per the
[OBS Forums AI policy](https://obsproject.com/forum/threads/ai-policy-and-resource-considerations.194917/)).

