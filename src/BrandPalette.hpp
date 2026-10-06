// SPDX-FileCopyrightText: 2026 Kamayu B.V. <https://www.kamayu.nl>
// SPDX-License-Identifier: GPL-2.0-or-later

//=======================================================================
//
//  BrandPalette.cpp
//
//  Created: 20 sep 2026
//
//  Author: N.H. Elroy with support from Claude Code (Sonnet)
//
//  Abstract: common definitions for BabelStreamer styles (matches
//            website CSS
//
//=======================================================================

/**
 
    BabelStreamer's dialog colours. Matches to the CSS colours
    on the website. Values are Qt style-sheet strings, not QColor, because that is what almostevery use needs.
    They are wrapped in QColor where a real colour object is wanted .
 **/

#pragma once

namespace brandpalette {

/** Surfaces and text */
constexpr const char *kBckg = "#0A1420";     // dialog background
constexpr const char *kPanel = "#102436";    // recessed panel (mono/code box)
constexpr const char *kText = "#EAF3F2";     // primary text
constexpr const char *kMuted = "#7E96A2";    // secondary text
constexpr const char *kTopText = "#0D1C2B";  // text on an accent-filled button
constexpr const char *kDisabled = "#4a5b66"; // a disabled control's text

/** Hairline borders, and hover tint **/
constexpr const char *kHairline = "rgba(255,255,255,0.08)";
constexpr const char *kHoverTint = "rgba(255,255,255,0.08)";

/** Accents - changes between popup kind **/
constexpr const char *kAccentTeal = "#3FE0C5";  // normal / success
constexpr const char *kAccentAmber = "#FFC65C"; // warning
constexpr const char *kAccentCoral = "#FF6F86"; // error / disconnected

} // namespace brandpalette
