// SPDX-FileCopyrightText: 2026 Kamayu B.V. <https://www.kamayu.nl>
// SPDX-License-Identifier: GPL-2.0-or-later

//=======================================================================
//
//  DeviceLinkDialog.hpp
//
//  Created: 20 sep 2026
//
//  Author: N.H. Elroy with support from Claude Code (Sonnet)
//
/// Abstract: Dialogue for handling connection of plugin to account. Needed
///         in order to access account based functions of translatoion and caption
///         distribution.
//
//=======================================================================

#pragma once

#include <functional>
#include <string>

/**
    Helper to activate the workflow to connect to account. If user says Yes
    the the connectAccountViaDeviceCode function will be called
 */
bool askToConnectAccount();

/**
    Run the connect-to-account process through the dialogue. This should be called from the Qt UI thread.
    The dialogue contains a "connect account" button which will link into the account
    sign-in page on the website.  The dialogue blocks until pairing completes, the code expires, or the user cancels.
 
    Workflow is:

     - POST /device/code to the website-> show the short user code + a "open sign-in page" button
     - poll POST /device/token to the website until the streamer approves it
     - hand the resulting device token back via onComplete.
 
    @param apiBase base parameters to pass to the wss api on the server. Format is "<scheme>://<host>[:<port>]/api"
    @param onComplete completion callback. token is the account token and label is the (unused) device name
 */
void connectAccountViaDeviceCode(const std::string &apiBase,
				 std::function<void(const std::string &token, const std::string &label)> onComplete);
