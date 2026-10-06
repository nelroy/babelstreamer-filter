// SPDX-FileCopyrightText: 2026 Kamayu B.V. <https://www.kamayu.nl>
// SPDX-License-Identifier: GPL-2.0-or-later

//=======================================================================
//
//  FilterOrigin.cpp
//
//  Created: 28 sep 2026
//
//  Author: N.H. Elroy with support from Claude Code (Sonnet)
//
//  Dev note: The resolution of server name is a partial duplication
//            of other modules. Needs to be reviewed
//
//=======================================================================

#include "FilterOrigin.hpp"
#include "WhisperFilter.hpp"

#include <obs-module.h>

#include <cstring>

namespace {

// The filter's own type id (WhisperFilter.cpp's obs_source_info.id).
constexpr const char *kFilterId = "whisper_ws_filter";

// Production origin, used when no filter has a server address configured yet.
constexpr const char *kDefaultOrigin = "https://babelstreamer.com";


// Get the first filter - which will also be the master filter
// findFirstFilter is the callback for obs_source_enum_filters
void findFirstFilter(obs_source_t *, obs_source_t *filter, void *param)
{
	auto *found = static_cast<obs_source_t **>(param);
	if (*found)	return;
	const char *id = obs_source_get_id(filter);
	if (id && std::strcmp(id, kFilterId) == 0)	*found = filter;
}


obs_source_t *firstFilterSource()
{
	obs_source_t *found = nullptr;
	obs_enum_sources(
		[](void *param, obs_source_t *src) -> bool
                     {
			obs_source_enum_filters(src, findFirstFilter, param);
			return !*static_cast<obs_source_t **>(param); // stop once found
		},
		&found);
	return found;
}

} // namespace


QString resolveFilterOrigin()
{
	obs_source_t *filter = firstFilterSource();
	if (!filter) return QString::fromUtf8(kDefaultOrigin);

	obs_data_t *settings = obs_source_get_settings(filter);
	QString host = QString::fromUtf8(obs_data_get_string(settings, S_WS_HOST)).trimmed();
	// parseWsHostInput() normally stores a bare host, but strip a scheme
	// defensively in case an older saved value still carries one.
	for (const char *prefix : {"wss://", "ws://", "https://", "http://"})
    {
		if (host.startsWith(QLatin1String(prefix), Qt::CaseInsensitive))
        {
			host = host.mid(int(std::strlen(prefix)));
			break;
		}
	}
	host = host.section(QLatin1Char('/'), 0, 0); // drop any path
	const int port = int(obs_data_get_int(settings, S_WS_PORT));
	const bool tls = obs_data_get_bool(settings, S_WS_USE_TLS);
	obs_data_release(settings);

	if (host.isEmpty()) return QString::fromUtf8(kDefaultOrigin);

	const QString scheme = tls ? QStringLiteral("https") : QStringLiteral("http");
	const int defaultPort = tls ? 443 : 80;
	QString authority = host;
	if (port > 0 && port != defaultPort) authority += QStringLiteral(":%1").arg(port);
	return QStringLiteral("%1://%2").arg(scheme, authority);
}

bool isMasterFilterSource(obs_source_t *source)
{
	return source && firstFilterSource() == source;
}
