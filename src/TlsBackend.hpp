// SPDX-FileCopyrightText: 2026 Kamayu B.V. <https://www.kamayu.nl>
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
// Shared by ModelDownloadDialog.cpp and UpdateChecker.cpp — both make plain
// QNetworkAccessManager HTTPS requests, and both need this called once
// before their first request.

#include "WhisperFilter.hpp" // VAD_MODEL_FILENAME

#include <obs-module.h>

#include <QCoreApplication>
#include <QFileInfo>

// OBS's own Qt6 deployment ships QtNetwork but strips every TLS backend
// plugin on both macOS and Windows (OBS itself never does HTTPS via
// QNetworkAccessManager) — any TLS handshake attempted from inside the OBS
// process otherwise fails with "TLS initialization failed", since Qt has no
// backend to hand the socket to. Bundled ourselves in data/tls/
// (libqsecuretransportbackend.dylib / qschannelbackend.dll — native
// SecureTransport/Schannel backends, no extra runtime dependency on either
// platform) and pointed at here via addLibraryPath(): Qt's plugin loader
// expects backends under a literal "tls" subfolder of an added library
// path, so this resolves the *parent* of a known-bundled file (the VAD
// model, guaranteed present on both platforms) rather than hardcoding a
// path per platform.
inline void ensureTlsBackendAvailable()
{
	static bool added = false;
	if (added)
		return;
	added = true;

	if (char *vadPath = obs_module_file(VAD_MODEL_FILENAME)) {
		const QFileInfo info(QString::fromUtf8(vadPath));
		bfree(vadPath);
		QCoreApplication::addLibraryPath(info.absolutePath());
	}
}
