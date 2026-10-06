// SPDX-FileCopyrightText: 2026 Kamayu B.V. <https://www.kamayu.nl>
// SPDX-License-Identifier: GPL-2.0-or-later

//=======================================================================
//
//  BrandedPopup.hpp
//
//  Created: 20 sep 2026
//
//  Author: N.H. Elroy with support from Claude Code (Sonnet)
//
/// Abstract:   Create a popup box with BabelStreamer branding. This should be used for most
///         popups to ensure that the BabelStreamer branding is visible to users
//
//=======================================================================

/**
    A styled popup dialogue which reproduces the BabelStreamer site colour scheme and fish logo
*/
#pragma once

#include <string>

/**
    enumn defining various roles fo popup dialogues. Definitions are self eviodent
 */
enum class PopupKind {
	SessionCreated,
	UsageWarning,
	SessionDisconnected,
	NoModelSelected,
	UpdateAvailable,
};

/**
    Function to show the branded BabelStreamer popup. This is the correct way to show the popup, as the
    popups need to be queued to a separate thread in order to correctly support downloading etc.
     
     @param kind                  type of popup dialogue (@see PopupKind)
     @param title                   dialogue box title string
     @param bodyText          text for body of dialogue
     @param sessionKey      optional copyable session code in the dialogue - adds code for copying and viewing the session key
     @param modelURL        adds an actionable URL to download a model and changes the "OK" button to a "Download" button
     @param parent               parent widget for this dialogue
 */
void showBabelStreamerPopup(PopupKind kind, const std::string &title, const std::string &body,
			    const std::string &copyValue = std::string(), const std::string &actionUrl = std::string());
