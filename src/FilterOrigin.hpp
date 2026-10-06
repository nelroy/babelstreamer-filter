// SPDX-FileCopyrightText: 2026 Kamayu B.V. <https://www.kamayu.nl>
// SPDX-License-Identifier: GPL-2.0-or-later

//=======================================================================
//
//  FilterOrigin.hpp
//
//  Created: 28 sep 2026
//
//  Author: N.H. Elroy with support from Claude Code (Sonnet)
//
///  Abstract:  Shared functions to resolve master filter and host  name for downloads from
///          the BabelStreamer website e.g. tokens and vocab lists
//
//=======================================================================

#pragma once

#include <QString>

#include <obs-module.h>

/**
    The configured host's reference ("<scheme>://<host>[:<port>]"), read from
    the first BabelStreamer filter found in the current scene collection, or
    the production origin ("https://babelstreamer.com") if none is configured
    yet. Must run on UI thread.

    @returns the  origin — callers append their own path
 */
QString resolveFilterOrigin();

/**
    Check if the given source filter is the master filter, Must run on the UI thread,

    @param source the filter's own obs_source_t (WhisperFilter::context)
    @returns true if source is the first BabelStreamer filter in OBS's
             own enumeration order
 */
bool isMasterFilterSource(obs_source_t *source);
