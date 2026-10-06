// SPDX-FileCopyrightText: 2026 Kamayu B.V. <https://www.kamayu.nl>
// SPDX-License-Identifier: GPL-2.0-or-later

//=======================================================================
//
//  UpdateChecker.hpp
//
//  Created: 20 sep 2026
//
//  Author: N.H. Elroy with support from Claude Code (Sonnet)
//
/// Abstract: Perform check on updated plugin version
//
//=======================================================================

#pragma once
/**
    Check for updates to a newer version of the plugin.
 
    Once the scene collection has finished loading, does an async GET to the server api
    (<server>/api/plugin/version falling back to default babelstreramer.com.
 
    If response is higher than current installed version then shows a popup with a link
    to download the latest version from the site.
*/
void checkForPluginUpdate();
