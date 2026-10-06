// SPDX-FileCopyrightText: 2026 Kamayu B.V. <https://www.kamayu.nl>
// SPDX-License-Identifier: GPL-2.0-or-later

//=======================================================================
//
//  GameVocab.cpp
//
//  Created: 24 sep 2026
//
//  Author: N.H. Elroy with support from Claude Code (Sonnet).
//
//=======================================================================

#include "GameVocab.hpp"
#include "FilterOrigin.hpp"
#include "WhisperFilter.hpp"
#include "plugin-support.h"

#include <obs-module.h>
#include <util/platform.h>

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

#include <atomic>
#include <fstream>
#include <mutex>

namespace
{

// Cache filename inside the plugin's OBS config directory.
const char *const kCacheFilename = "game_vocab_cache.json";

// Guards the cached vocabulary.
std::mutex cacheMutex;

// The whole server response, cached verbatim - games in server order, each
// with its "name" and its "terms" array of {"term": "..."} objects.
QJsonArray cachedGames;

// True once the cache file has been read, so it is only loaded once per session.
bool cacheLoadedFromDisk = false;

// Guard against multiple simultaneous vocab fetches from multiple filters
std::atomic<bool> fetchInFlight{false};

// Get the full path to the cache dir from OBS. May be empty if OBS refuses one
std::string cacheFilePath()
{
	char *dir = obs_module_config_path(nullptr);
	if (!dir) return {};

	std::string path(dir);
	bfree(dir);
    
    // fix up trailing slashes
	if (!path.empty() && path.back() != '/' && path.back() != '\\')
		path += '/';
	path += kCacheFilename;
	return path;
}

/*
    Read the cache file into cachedGames. File may not exist.
  This should be called with the cache locked via cacheMutex
 */
void loadCacheLocked()
{
	if (cacheLoadedFromDisk) return;
	cacheLoadedFromDisk = true;

	const std::string path = cacheFilePath();
	if (path.empty()) return;

	std::ifstream in(path, std::ios::binary);
	if (!in) return;

	const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	cachedGames = QJsonDocument::fromJson(QByteArray::fromStdString(text))
			      .object()
			      .value(QStringLiteral("games"))
			      .toArray();
}

/*
    Write cachedGames back to disk verbatim, wrapped the same way the server
    sends it. This should be called with the cache locked via cacheMutex
 */
void saveCacheLocked()
{
	char *dir = obs_module_config_path(nullptr);
	if (dir)
    {
		os_mkdirs(dir);
		bfree(dir);
	}

	const std::string path = cacheFilePath();
	if (path.empty()) return;

	QJsonObject root;
	root.insert(QStringLiteral("games"), cachedGames);

	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (out) out << QJsonDocument(root).toJson(QJsonDocument::Compact).toStdString();
}

/*
    Find the terms array for one game name within cachedGames. Returns
    list of terms, which may be empty. This should be called with the cache locked
 via cacheMutex
 */
QJsonArray findTermsLocked(const std::string &gameName)
{
	const QString wanted = QString::fromStdString(gameName);
	for (const QJsonValue entry : cachedGames)
    {
		const QJsonObject game = entry.toObject();
		if (game.value(QStringLiteral("name")).toString() == wanted)
			return game.value(QStringLiteral("terms")).toArray();
	}
	return {};
}

/*
    Helper to run a vocab fetcher on the Qt main thread
 */
void runOnMainThread(std::function<void()> work)
{
	QTimer::singleShot(0, QCoreApplication::instance(), std::move(work));
}

} // namespace


// Externally visible game list getter
std::vector<std::string> cachedGameNames()
{
	std::lock_guard<std::mutex> lock(cacheMutex);
	loadCacheLocked();

	std::vector<std::string> names;
	names.reserve(cachedGames.size());
	for (const QJsonValue entry : cachedGames)
    {
		const QString name = entry.toObject().value(QStringLiteral("name")).toString();
		if (!name.isEmpty())
			names.push_back(name.toStdString());
	}
	return names;
}

// Externally visible game term getter
std::string cachedGameTerms(const std::string &gameName)
{
	if (gameName.empty()) return {};

	std::lock_guard<std::mutex> lock(cacheMutex);
	loadCacheLocked();

	std::string joined;
	for (const QJsonValue entry : findTermsLocked(gameName))
    {
		const QString term = entry.toObject().value(QStringLiteral("term")).toString();
		if (term.isEmpty())
			continue;
		if (!joined.empty())
			joined += ", ";
		joined += term.toStdString();
	}
	return joined;
}

// externally visible downloader for game vocab
void refreshGameVocabulary()
{
	if (fetchInFlight.exchange(true))
		return;

	runOnMainThread([]() {
		const QString url = resolveFilterOrigin() + QStringLiteral("/game-vocab.json");

		QNetworkRequest request{QUrl(url)};
		request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::NoLessSafeRedirectPolicy);
        
		// Named and versioned so we can see this is a plugin call, rather than a random request
		request.setHeader(QNetworkRequest::UserAgentHeader,
				  QStringLiteral("BabelStreamer-OBS-Plugin/%1").arg(QString::fromUtf8(PLUGIN_VERSION)));

		auto *manager = new QNetworkAccessManager();
		QNetworkReply *reply = manager->get(request);

		QObject::connect(reply, &QNetworkReply::finished, manager, [manager, reply]() {
			reply->deleteLater();
			manager->deleteLater();
			fetchInFlight.store(false);

			if (reply->error() != QNetworkReply::NoError)
            {
				blog(LOG_INFO, "[babelstreamer-filter] Game vocabulary refresh failed: %s",
				     reply->errorString().toUtf8().constData());
				return;
			}

			const QJsonArray games = QJsonDocument::fromJson(reply->readAll())
							 .object()
							 .value(QStringLiteral("games"))
							 .toArray();
			if (games.isEmpty()) return;

			std::lock_guard<std::mutex> lock(cacheMutex);
			cachedGames = games;
			saveCacheLocked();
			blog(LOG_INFO, "[babelstreamer-filter] Refreshed game vocabulary: %lld games",
			     (long long)cachedGames.size());
		});
	});
}
