// SPDX-FileCopyrightText: 2026 Kamayu B.V. <https://www.kamayu.nl>
// SPDX-License-Identifier: GPL-2.0-or-later

//=======================================================================
//
//  sha256.h
//
//  Created: 20 sep 2026
//
//  Author: N.H. Elroy with support from Claude Code (Sonnet)
//
///  Abstract: Public-domain SHA-256 (adapted from Brad Conte's  github.com/B-Con/crypto-algorithms
///         implementation)
//
//=======================================================================

#pragma once

#include <string>

/**
    Calculate the 64-character lowercase hex SHA-256 digest of `input`
 
    @param input input text string
 
    @returns SHA-256 digest
 */
std::string sha256Hex(const std::string &input);
