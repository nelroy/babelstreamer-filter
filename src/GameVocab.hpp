// SPDX-FileCopyrightText: 2026 Kamayu B.V. <https://www.kamayu.nl>
// SPDX-License-Identifier: GPL-2.0-or-later

//=======================================================================
//
//  GameVocab.hpp
//
//  Created: 24 sep 2026
//
//  Author: N.H. Elroy with support from Claude Code (Sonnet)
//
///  Abstract: Per-game vocabulary used to prime Whisper, fetched from
///          babelstreamer.com and held in a local cache. Replaces the
///          game-vocab.json that used to ship inside the plugin.
//
//=======================================================================

#pragma once

#include <string>
#include <vector>

/**
    Game names for the Game dropdown, read from the local cache.

    Never blocks and never touches the network, because the OBS properties
    dialog builds its widgets synchronously. Returns empty until the first
    successful refresh has landed.

    @returns game names in the order the server supplied them
 */
std::vector<std::string> cachedGameNames();

/**
    One game's priming terms, comma-joined, read from the local cache.

    Pure cache read — does not trigger a fetch. refreshGameVocabulary()
    fetches every game's terms at once, so by the time a streamer has picked
    a game the terms are already cached or they are not coming.

    @param gameName  game to look up, matching a name from cachedGameNames()

    @returns comma-joined terms, or empty if not cached
 */
std::string cachedGameTerms(const std::string &gameName);

/**
    Refresh the cached game vocabulary from babelstreamer.com.

    Safe to call from any thread and safe to call repeatedly; a fetch already
    in flight is not duplicated.
 */
void refreshGameVocabulary();
