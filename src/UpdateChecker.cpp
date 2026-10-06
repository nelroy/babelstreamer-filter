// SPDX-FileCopyrightText: 2026 Kamayu B.V. <https://www.kamayu.nl>
// SPDX-License-Identifier: GPL-2.0-or-later

//=======================================================================
//
//  UpdateChecker.cpp
//
//  Created: 20 sep 2026
//
//  Author: N.H. Elroy with support from Claude Code (Sonnet)
//
//=======================================================================

#include "UpdateChecker.hpp"

#include "BrandedPopup.hpp"
#include "FilterOrigin.hpp"
#include "TlsBackend.hpp" // ensureTlsBackendAvailable()

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStringList>
#include <QUrl>

#include <array>

namespace {

// Split the version into primary, secondary and build versions
std::array<int, 3> parseVersion(const QString &v)
{
	std::array<int, 3> parts{0, 0, 0};
	const QStringList segs = v.split(QLatin1Char('.'));
	for (int i = 0; i < 3 && i < segs.size(); ++i)
		parts[i] = segs[i].toInt();
	return parts;
}

/*
    Get the actual URL to check the version on.
 */
QString resolveVersionCheckUrl()
{
	return resolveFilterOrigin() + QStringLiteral("/api/plugin/version");
}

/*
    Run tha actual update check. Contact the server and, if there is a newer version
    put up the dialogue to download that version. It doesn't actually install and restart
    OBS, justs downloads.
 */
void runUpdateCheck(void *)
{
	ensureTlsBackendAvailable();

	// Get the URL (more involved than you might think)
	const QString url = resolveVersionCheckUrl();
	blog(LOG_INFO, "[babelstreamer-filter] Checking for updates at %s", url.toUtf8().constData());

	// put out the request to the BabelStreamer server
	auto *nam = new QNetworkAccessManager();
	QNetworkRequest req{QUrl(url)};
	req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
	QNetworkReply *reply = nam->get(req);

	QObject::connect(reply, &QNetworkReply::finished, nam, [nam, reply]() {
		reply->deleteLater();
		nam->deleteLater();

		// Silent on any failure - a background update nag has no
		// business bothering the user about its own network error.
		if (reply->error() != QNetworkReply::NoError) {
			blog(LOG_INFO, "[babelstreamer-filter] Update check failed: %s",
			     reply->errorString().toUtf8().constData());
			return;
		}

		// If we have the latest version or no value given, no worries
		const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
		const QString latest = obj.value(QStringLiteral("latest")).toString();
		if (latest.isEmpty())
			return;

		const QString current = QString::fromUtf8(PLUGIN_VERSION);
		if (!(parseVersion(current) < parseVersion(latest)))
			return;

		// At this point we need a dialogue to point the user at the latest download
#ifdef _WIN32
		const QString urlKey = QStringLiteral("urlWin");
#else
        const QString urlKey = QStringLiteral("urlMac");
#endif
		const QString dlUrl = obj.value(urlKey).toString();
		const QString notes = obj.value(QStringLiteral("notes")).toString();
		if (dlUrl.isEmpty())
			return;

		QString body = QStringLiteral("Version %1 is available (you have %2).").arg(latest, current);
		if (!notes.isEmpty())
			body += QStringLiteral("\n\n%1").arg(notes);
		body += QStringLiteral("\n\nAfter installing, restart OBS for the update to take effect.");

		showBabelStreamerPopup(PopupKind::UpdateAvailable, "Update Available", body.toStdString(),
				       std::string(), dlUrl.toStdString());
	});
}

/*
    Runs on an OBS event. If the Scenes have finished loading (which means all filters are known)
    then queues the update check aysnchronously
 */
void onFrontendEvent(enum obs_frontend_event event, void *)
{
	if (event != OBS_FRONTEND_EVENT_FINISHED_LOADING)
		return;
	obs_frontend_remove_event_callback(onFrontendEvent, nullptr);
	obs_queue_task(OBS_TASK_UI, runUpdateCheck, nullptr, false);
}

} // namespace

/*
    Schedule the check for when an OBS event happens
 */
void checkForPluginUpdate()
{
	obs_frontend_add_event_callback(onFrontendEvent, nullptr);
}
