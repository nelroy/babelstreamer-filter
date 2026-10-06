// SPDX-FileCopyrightText: 2026 Kamayu B.V. <https://www.kamayu.nl>
// SPDX-License-Identifier: GPL-2.0-or-later

//=======================================================================
//
//  WhisperEngine.hpp
//
//  Created: 26 sep 2026
//
//  Author: N.H. Elroy with support from Claude Code (Sonnet)
//
///  Abstract: Turns a WhisperFilter's settings into a running  WhisperWrapper — model load,
///          live-config hot-swap, and the Apple Neural Engine / Core ML encoder plumbing.
//
//=======================================================================

#pragma once

#include "WhisperWrapper.h"

#include <filesystem>
#include <string>

struct WhisperFilter;

/**
    (Re)build f's WhisperWrapper from its current settings — model path,
    device, language, VAD/partials config, and (on Apple Silicon Core ML
    builds) the Neural Engine encoder symlink. No-op if f->modelPath is empty.
 */
void startWhisper(WhisperFilter *f);

/**
    The subset of settings a running WhisperWrapper can hot-swap without a
    full reload (see WhisperWrapper::LiveConfig) — shared by startWhisper()
    and filterUpdate()'s no-reload path so the two can't drift.
 */
WhisperWrapper::LiveConfig makeLiveConfig(const WhisperFilter *f);

/**
    The device id actually handed to WhisperWrapper, and therefore the one
    the shared-model registry is keyed on. "Apple Neural Engine" is not a
    ggml backend: it means the ENCODER runs on the ANE while the decoder runs
    on the CPU, so the effective device for ANE is "cpu".
 */
std::string effectiveDeviceId(const WhisperFilter *f);

#if HAS_WHISPER_COREML
/**
    True on Apple-Silicon hardware (every M-series has a Neural Engine).
    Reads the CPU capability rather than the compiled arch, so it's correct
    across the universal binary's slices and under Rosetta.
 */
bool isAppleSilicon();

/**
    The path whisper probes for modelPath's Core ML encoder: whisper's own
    derived name, next to the model's .bin. Used to detect whether an ANE
    prompt is still needed (see checkAnePairing/aneAskTask in
    WhisperFilter.cpp) — the actual symlink management lives in
    ensureCoremlAdjacency(), private to WhisperEngine.cpp.
 */
std::filesystem::path coremlAdjacentPath(const std::string &modelPath);
#endif
