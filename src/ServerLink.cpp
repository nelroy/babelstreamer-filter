// SPDX-FileCopyrightText: 2026 Kamayu B.V. <https://www.kamayu.nl>
// SPDX-License-Identifier: GPL-2.0-or-later

//=======================================================================
//
//  ServerLink.cpp
//
//  Created: 26 sep 2026
//
//  Author: N.H. Elroy with support from Claude Code (Sonnet)
//
//  Dev Note: This singleton takes queued text and despatches it to the
//            BabelStreamer server. It also handles handshakes with the
//            server and resolution of session keys, as well as reacting
//            to changes in streaming status by opening/closing the link
//
//            The following threads are used:
//
//                  N whisper workers   - enqueue() only
//                  1 sender thread     - ALL socket I/O (connect, handshake, server
//                                        frames, transcript sends)
//                  UI thread           - add/update/remove/setActive/sessionKey
//                  OBS frontend thread - frontendEvent (streaming start/stop)
//
//      linkMutex guards every member below except isStreaming_, which is
//      atomic
//
//=======================================================================

#include "ServerLink.hpp"
#include "BrandedPopup.hpp"
#include "FilterOrigin.hpp" // isMasterFilterSource
#include "WebSocketClient.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include "sha256.h"
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

/* Extract a quoted string value from a minimal JSON object

    json: JSON string to use
    key: name of key to look for

    returns value if found, otherwise empty string

    example: extractJsonString ({"type":"session","key":"ABC123"}, "key")
    returns "ABC123"
 */

static std::string extractJsonString(const std::string &json, const std::string &field)
{
	const std::string needle = "\"" + field + "\"";
	auto pos = json.find(needle);
	if (pos == std::string::npos)
		return {};
	pos = json.find('"', pos + needle.size()); // opening quote of value
	if (pos == std::string::npos)
		return {};
	++pos; // step past opening quote

	std::string result;
	for (; pos < json.size(); ++pos) {
		char c = json[pos];
		if (c == '"')
			return result; // unescaped closing quote
		if (c == '\\' && pos + 1 < json.size()) {
			char next = json[++pos];
			switch (next) {
			case '"':
				result += '"';
				break;
			case '\\':
				result += '\\';
				break;
			case '/':
				result += '/';
				break;
			case 'n':
				result += '\n';
				break;
			case 't':
				result += '\t';
				break;
			case 'r':
				result += '\r';
				break;
			case 'b':
				result += '\b';
				break;
			case 'f':
				result += '\f';
				break;
			default:
				result += next;
				break; // unknown escape (\u...): keep literally
			}
			continue;
		}
		result += c;
	}
	return {}; // unterminated string
}

/*
    Show the sesosion created popup. The server response also contains a message
    about session tim remaining (usage message). The key is a session key which
    should normally remain unchanged for a given machine id
 */
static void popupSessionCreated(const std::string &key, const std::string &usageMessage)
{
	std::string body = "Copy the session key and share it with your viewers.";
	if (!usageMessage.empty())
		body += "\n\n" + usageMessage;
	showBabelStreamerPopup(PopupKind::SessionCreated, "Session Created", body, key);
}

/*
    when under an hour of translation time remains this billing period (server-side
    one-shot guard) then warn the user - once per session only
 */
static void popupUsageWarning(const std::string &message)
{
	if (message.empty())
		return;
	showBabelStreamerPopup(PopupKind::UsageWarning, "Usage Warning", message);
}

/*
    Inform about a disconnect. The server ended a previously-live session (monthly cap reached,
    subscription lapsed), the connection was lost outright, or the very first connect attempt
    was rejected (bad/revoked/missing device token) - check on the message itself to ensure
    only once per session. Fires any time the problem occurs and whenever the situation changes
 */
static void popupSessionDisconnected(const std::string &message)
{
	const std::string body = message.empty() ? "The connection to the BabelStreamer server was lost." : message;
	showBabelStreamerPopup(PopupKind::SessionDisconnected, "Session Disconnected", body);
}

/*
    Get a consistent probably unique device fingerprint for identifying this device
    Not cached, but computed (low cost) whenever needed. Only used and communicated
    in hashed version. On Windows this is the MachineGuid registry key, on MacOS
    the IOPlatformUUID.
 */
static std::string rawMachineId()
{
#ifdef _WIN32
	HKEY key;
	if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography", 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY,
			  &key) != ERROR_SUCCESS)
		return "";

	char buf[64] = {0};
	DWORD bufLen = sizeof(buf);
	DWORD type = 0;
	LONG result = RegQueryValueExA(key, "MachineGuid", nullptr, &type, reinterpret_cast<LPBYTE>(buf), &bufLen);
	RegCloseKey(key);
	if (result != ERROR_SUCCESS || type != REG_SZ)
		return "";
	return std::string(buf);
#elif defined(__APPLE__)
	io_registry_entry_t entry = IORegistryEntryFromPath(kIOMainPortDefault, "IOService:/");
	if (entry == 0)
		return "";

	CFStringRef uuidRef = static_cast<CFStringRef>(
		IORegistryEntryCreateCFProperty(entry, CFSTR("IOPlatformUUID"), kCFAllocatorDefault, 0));
	IOObjectRelease(entry);
	if (!uuidRef)
		return "";

	char buf[64] = {0};
	bool ok = CFStringGetCString(uuidRef, buf, sizeof(buf), kCFStringEncodingUTF8);
	CFRelease(uuidRef);
	return ok ? std::string(buf) : "";
#else
	return "";
#endif
}

// get the raw machine id and hash to sha256
static std::string computeDeviceFingerprint()
{
	std::string rawId = rawMachineId();
	if (rawId.empty())
		return "";
	return sha256Hex(rawId);
}

/*
    ServerLink is a Myers singleton class which manages the access to server i/o and
    serialises requests to use i/o. Filters push outgoing i/o onto a FIFO which
    is drained by a worker thread.

    ServerLink uses the following threads

    - one worker thread per filter to asynchronously enque i/o
    - one sender thread which drains the i/o FIFO
    - a separate thread to manage the UI
    - a thread communicate with OBS to detect stream start/stop

    The Singleton pattern is implemented by keeping the constructor private
    and instantiating the class statically within instance. The only way to
    access the class is through ServerLink::instance().

    Kept private to this file - ServerLink.hpp exposes only the free
    serverLink*() functions below, the same Pimpl-style split DeviceLinkDialog
    uses, so nothing outside this file can reach into the threading internals.

    Ownership of the connection settings follows the same "master filter"
    concept as the plugin-wide device/model settings (see
    FilterOrigin.hpp's isMasterFilterSource()): the master's params are
    adopted, every other client's are compared and just logged if they
    disagree.
 */
class ServerLink::Pimpl {
public:
	/*
        Add a client (filter) to the list of clients. If this is the first
        client we take the opportunity to build up the socket. adoptParamsLocked()
        works out for itself whether this new client is the master (OBS's
        source/filter order, not registration order - a filter can be added
        that turns out to sit earlier in the UI than one already registered),
        so it runs unconditionally rather than only on the very first add.
     */
	ServerLinkId add(ServerLinkParams p)
	{
		ServerLinkId id;
		bool first = false;
		{
			std::lock_guard<std::mutex> lk(linkMutex);
			id = nextId++;
			clientList.push_back({id, std::move(p), true});
			first = (clientList.size() == 1);
			if (first)
				serverSocketClient = std::make_unique<WebSocketClient>();
			adoptParamsLocked();
			warnOnParamMismatchLocked(clientList.back());
		}
		if (first) {
			senderThread = std::thread(&Pimpl::senderLoop, this);
			obs_frontend_add_event_callback(&Pimpl::frontendEvent, nullptr);
		}
		return id;
	}

	/*
        Update settings on a registered filter. Fail silently if filter
        not registered. Always re-evaluates who's master, same reasoning
        as add() above.
     */
	void update(ServerLinkId id, ServerLinkParams p)
	{
		{
			std::lock_guard<std::mutex> lk(linkMutex);
			auto it = findLocked(id);
			if (it == clientList.end())
				return;
			it->p = std::move(p);
			adoptParamsLocked();
			warnOnParamMismatchLocked(*it);
		}
		linkConditionVariable.notify_one();
	}

	/*
        Remove a client from the list - graceful teardown when deleting
        a filter instance. One little quirk - if the deleted filter
        was the master we need to nominate a new one, same as add()/update():
        adoptParamsLocked() re-derives who's master from OBS's own filter
        order among whoever's left.
     */
	void remove(ServerLinkId id)
	{
		// remove the filter
		bool last = false;
		{
			std::lock_guard<std::mutex> lk(linkMutex);
			auto it = findLocked(id);
			if (it == clientList.end())
				return;
			clientList.erase(it);
			if (clientList.empty()) {
				senderStop = true;
				last = true;
			} else {
				// The removed filter may have been the master; whoever is
				// now first in OBS's filter order takes over.
				adoptParamsLocked();
			}
		}

		// tell everybody in case they have business to attend to
		linkConditionVariable.notify_all();

		// if this was the last one then tear down - essentially an implementation
		// of reference counting
		if (last) {
			if (senderThread.joinable())
				senderThread.join();
			obs_frontend_remove_event_callback(&Pimpl::frontendEvent, nullptr);
			if (serverSocketClient)
				serverSocketClient->disconnect();
			serverSocketClient.reset();
			std::lock_guard<std::mutex> lk(linkMutex);
			senderStop = false;
			dropConnection = false;
			outboxQueue.clear();
			sessionKey_.clear();
			lastReportedProblem.clear();
		}
	}

	/*
        Activate/Deactivate filter. This is a reponse to an OBS
        activation event
     */
	void setActive(ServerLinkId id, bool active)
	{
		std::vector<std::function<void()>> overlays;
		{
			std::lock_guard<std::mutex> lk(linkMutex);
			auto clientInstance = findLocked(id);
			if (clientInstance == clientList.end() || clientInstance->active == active)
				return;
			clientInstance->active = active;
			if (active || anyActiveLocked())
				return;
			blog(LOG_INFO, "[babelstreamer-filter] No active filters left - closing the server connection");
			dropConnection = true;
			overlays = overlayClearersLocked();
		}
		linkConditionVariable.notify_one();
		runOverlayClearers(overlays);
	}

	/*
        Queue a text block for sending. Note the the outbox queue
        is bounded so overloads will just cause blocks to be missed
     */
	void enqueue(std::string json)
	{
		static constexpr size_t MAX_OUTBOX = 256;
		{
			std::lock_guard<std::mutex> lk(linkMutex);
			outboxQueue.push_back(std::move(json));
			while (outboxQueue.size() > MAX_OUTBOX) {
				outboxQueue.pop_front();
			}
		}
		linkConditionVariable.notify_one();
	}

	/*
        Hangs up if gating applies and OBS isn't live.  Also clears every
        filter's overlay: whisper stops running in this state, so any caption
        left on screen is stale and nothing will replace it.
     */
	void applyStreamGate()
	{
		std::vector<std::function<void()>> overlays;
		{
			std::lock_guard<std::mutex> lk(linkMutex);
			if (!effectiveGateLocked() || isStreaming_.load())
				return;
			dropConnection = true;
			overlays = overlayClearersLocked();
		}
		blog(LOG_INFO, "[babelstreamer-filter] Not live - closing the server connection");
		linkConditionVariable.notify_one();
		runOverlayClearers(overlays);
	}

	/* status of streaming (obviously) */
	void setIsStreaming(bool live) { isStreaming_.store(live); }
	bool isStreaming() const { return isStreaming_.load(); }

	std::string sessionKey()
	{
		std::lock_guard<std::mutex> lk(linkMutex);
		return sessionKey_;
	}

	/*
        How many filters are active? We need to know to decide
        if we need to add the name to captions
     */
	size_t activeCount()
	{
		std::lock_guard<std::mutex> lk(linkMutex);
		size_t count = 0;
		for (const auto &c : clientList)
			if (c.active)
				++count;
		return count;
	}

	void clearAllOverlays()
	{
		std::vector<std::function<void()>> overlays;
		{
			std::lock_guard<std::mutex> lk(linkMutex);
			overlays = overlayClearersLocked();
		}
		runOverlayClearers(overlays);
	}

	/*
        STARTED (not STARTING) gates the connect side, so a stream that fails to
        go live never opens a billed connection; STOPPING (not STOPPED) gates the
        disconnect side, so the connection drops the instant the streamer clicks
        Stop rather than waiting for output teardown to finish.
     */
	static void frontendEvent(enum obs_frontend_event event, void *)
	{
		// Nested class: may reach the singleton's private pimpl directly.
		Pimpl &link = *ServerLink::getInstance().pimpl;
		if (event == OBS_FRONTEND_EVENT_STREAMING_STARTED) {
			link.isStreaming_.store(true);
			blog(LOG_INFO, "[babelstreamer-filter] Streaming started");
		} else if (event == OBS_FRONTEND_EVENT_STREAMING_STOPPING) {
			link.isStreaming_.store(false);
			blog(LOG_INFO, "[babelstreamer-filter] Streaming stopping");
			link.applyStreamGate();
		} else if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
			// Every source is now guaranteed to exist, including overlay text
			// sources that may not have loaded yet when filterCreate() ran and
			// tried to clear a stale caption.
			link.clearAllOverlays();
		}
	}

private:
	struct Client {
		ServerLinkId id;
		ServerLinkParams p;
		bool active;
	};

	std::vector<Client>::iterator findLocked(ServerLinkId id)
	{
		for (auto it = clientList.begin(); it != clientList.end(); ++it)
			if (it->id == id)
				return it;
		return clientList.end();
	}

	bool anyActiveLocked() const
	{
		for (const auto &c : clientList)
			if (c.active)
				return true;
		return false;
	}

	// The gate is the AND of every ACTIVE filter's checkbox
	bool effectiveGateLocked() const
	{
		if (!anyActiveLocked())
			return true;
		for (const auto &client : clientList) {
			if (client.active && !client.p.streamGateEnabled)
				return false;
		}
		return true;
	}

	std::vector<std::function<void()>> overlayClearersLocked() const
	{
		std::vector<std::function<void()>> out;
		out.reserve(clientList.size());
		for (const auto &c : clientList)
			if (c.p.clearOverlay)
				out.push_back(c.p.clearOverlay);
		return out;
	}

	static void runOverlayClearers(const std::vector<std::function<void()>> &cs)
	{
		for (const auto &c : cs)
			c();
	}

	/*
        Use the parameters as defined by the master filter. Should be locked
        before call.
     */
	void adoptParamsLocked()
	{
		if (clientList.empty())
			return;
		auto masterIt = std::find_if(clientList.begin(), clientList.end(),
					     [](const Client &c) { return isMasterFilterSource(c.p.source); });
		const ServerLinkParams &p = (masterIt != clientList.end()) ? masterIt->p : clientList.front().p;
		if (p.host == host_ && p.port == port_ && p.useTls == useTls_ && p.deviceToken == token_)
			return;
		const bool hadConnection = !host_.empty();
		host_ = p.host;
		port_ = p.port;
		useTls_ = p.useTls;
		token_ = p.deviceToken;
		sessionKey_.clear();
		if (hadConnection) {
			// Route through sender to avoid blocking UI thread
			dropConnection = true;
			blog(LOG_INFO, "[babelstreamer-filter] Server settings changed, will reconnect");
		}
	}

	/*
        Check that params match. Only relevant for the master filter as the
        others can't effect connection params.
     */
	void warnOnParamMismatchLocked(const Client &client) const
	{
		if (!isMasterFilterSource(client.p.source))
			return;
		if (client.p.host == host_ && client.p.port == port_ && client.p.useTls == useTls_ &&
		    client.p.deviceToken == token_)
			return;
		blog(LOG_WARNING,
		     "[babelstreamer-filter] The master filter's server settings (%s://%s:%u) differ from "
		     "the active connection (%s://%s:%u) - ignored.",
		     client.p.useTls ? "wss" : "ws", client.p.host.c_str(), (unsigned)client.p.port,
		     useTls_ ? "wss" : "ws", host_.c_str(), (unsigned)port_);
	}

	// Dedup on the message text, not on whether a session was live
	bool claimProblem(const std::string &message, bool clearSession)
	{
		std::lock_guard<std::mutex> lk(linkMutex);
		if (clearSession)
			sessionKey_.clear();
		if (message == lastReportedProblem)
			return false;
		lastReportedProblem = message;
		return true;
	}

	/*
        Sender thread only. Popups always run on their own detached thread so
	    this never blocks it, and shared fields are touched only under mtx_
     */

	void handleServerFrame(const std::string &frame)
	{
		const std::string type = extractJsonString(frame, "type");

		if (type == "session") {
			const std::string key = extractJsonString(frame, "key");
			if (key.empty())
				return;
			std::string previousKey;
			{
				std::lock_guard<std::mutex> lk(linkMutex);
				previousKey = sessionKey_;
				sessionKey_ = key;
				// A successful session means whatever was previously wrong is
				// no longer true - clear it so the SAME complaint recurring
				// later (the cap hit again next billing period) is shown again
				// rather than staying suppressed forever.
				lastReportedProblem.clear();
			}
			blog(LOG_INFO, "[babelstreamer-filter] Session key: %s", key.c_str());
			// The device token is stable, so the derived key is normally the
			// SAME one every time - only pop a dialog when it actually changes.
			if (key != previousKey) {
				const std::string usageMessage = extractJsonString(frame, "message");
				std::thread(popupSessionCreated, key, usageMessage).detach();
			}
			return;
		}

		if (type == "usage_warning") {
			const std::string message = extractJsonString(frame, "message");
			blog(LOG_INFO, "[babelstreamer-filter] Usage warning: %s", message.c_str());
			std::thread(popupUsageWarning, message).detach();
			return;
		}

		if (type == "error") {
			const std::string message = extractJsonString(frame, "message");
			blog(LOG_WARNING, "[babelstreamer-filter] Server error: %s", message.c_str());
			serverSocketClient->disconnect();
			if (claimProblem(message, /*clearSession=*/true))
				std::thread(popupSessionDisconnected, message).detach();
			return;
		}
	}

	/*
        Reads and dispatches anything the server has already pushed (a usage_warning,
        or a mid-stream cap/lapse error). Piggybacking on the per-transcript send
        means a pushed message is picked up the next time there's something to send
     */
	void drainPendingMessages()
	{
		for (int i = 0; i < 8; ++i) {
			std::string frame;
			if (!serverSocketClient->receiveText(frame, 5))
				break; // nothing pending, or a real failure
			handleServerFrame(frame);
		}

		if (!serverSocketClient->isConnected()) {
			const std::string message = "The connection to the BabelStreamer server was lost.";
			if (claimProblem(message, /*clearSession=*/true))
				std::thread(popupSessionDisconnected, message).detach();
		}
	}

	/*
        Ensure that we are connected to the server or fal. This blocks
        on a network wait so shouldn't be called from the UI or OBS thread
     */
	void ensureConnected()
	{
		if (serverSocketClient->isConnected()) {
			drainPendingMessages();
			return;
		}

		std::string host, token;
		uint16_t port = 0;
		bool useTls = false;
		{
			std::lock_guard<std::mutex> lk(linkMutex);
			host = host_;
			port = port_;
			useTls = useTls_;
			token = token_;
		}

		if (token.empty()) {
			const std::string message =
				"No device token configured - use \"Connect account\" in this filter's "
				"settings to link this OBS to your BabelStreamer account.";
			blog(LOG_WARNING, "[babelstreamer-filter] %s", message.c_str());
			if (claimProblem(message, /*clearSession=*/false)) {
				std::thread(popupSessionDisconnected, message).detach();
			}
			return;
		}

		/*
            Send the token. User headers rather thaמ URL query string as we
            use reverse proxy on the server and this may strip the token
            from plaintext access logs.
         */
		std::string fp = computeDeviceFingerprint();
		const std::vector<std::pair<std::string, std::string>> authHeaders = {
			{"X-Device-Token", token},
			{"X-Device-Fingerprint", fp},
		};
		if (!serverSocketClient->connect(host, port, "/", 3000, useTls, authHeaders)) {
			blog(LOG_WARNING, "[babelstreamer-filter] Cannot connect to %s://%s:%u", useTls ? "wss" : "ws",
			     host.c_str(), (unsigned)port);
			return;
		}

		/*
            Read the session (or rejection) frame the server sends immediately on
            connect - everything else is handled by handleServerFrame().
         */
		std::string frame;
		if (!serverSocketClient->receiveText(frame, 3000)) {
			blog(LOG_WARNING, "[babelstreamer-filter] Timed out waiting for session key");
			return;
		}
		handleServerFrame(frame);
	}

	/*
        All server traffic goes through here - loop sleeps and waits for something
        to send
     */
	void senderLoop()
	{
		constexpr auto kConnectBackoff = std::chrono::seconds(5);
		auto lastConnectFailure = std::chrono::steady_clock::time_point{};

		for (;;) {
			std::string msg;
			bool hangUp = false;
			bool gatedOff = false;
			{
				std::unique_lock<std::mutex> lk(linkMutex);
				linkConditionVariable.wait(lk, [&] {
					return senderStop || dropConnection || !outboxQueue.empty();
				});
				if (senderStop)
					break;
				hangUp = dropConnection;
				dropConnection = false;
				if (!outboxQueue.empty()) {
					msg = std::move(outboxQueue.front());
					outboxQueue.pop_front();
				}
				gatedOff = effectiveGateLocked() && !isStreaming_.load();
			}

			if (hangUp)
				serverSocketClient->disconnect();
			if (msg.empty())
				continue;

			if (!serverSocketClient->isConnected()) {
				// Gated off and not live: never open a new connection. A
				// transcript can still be sitting in the queue from right
				// before the gate closed - don't let it force a reconnect.
				if (gatedOff)
					continue;
				const auto now = std::chrono::steady_clock::now();
				if (now - lastConnectFailure < kConnectBackoff) {
					continue; // still backing off
				}
				ensureConnected();
				if (!serverSocketClient->isConnected()) {
					lastConnectFailure = now;
					continue; // connect failed
				}
			} else {
				ensureConnected(); // connected: just drains pushed server frames
			}

			if (serverSocketClient->isConnected() && !serverSocketClient->sendText(msg))
				blog(LOG_WARNING, "[babelstreamer-filter] Failed to send transcript");
		}
	}

	mutable std::mutex linkMutex;
	std::condition_variable linkConditionVariable;
	std::deque<std::string> outboxQueue;
	std::vector<Client> clientList;
	ServerLinkId nextId = 1;
	std::unique_ptr<WebSocketClient> serverSocketClient;
	std::thread senderThread;
	bool senderStop = false;
	bool dropConnection = false;
	std::string sessionKey_;
	std::string lastReportedProblem;
	// Live connection parameters, seeded from the master filter.
	std::string host_;
	uint16_t port_ = 0;
	bool useTls_ = false;
	std::string token_;
	std::atomic<bool> isStreaming_{false};
};

/*
    Singleton implemented by creating a static instance and then making
    constructor private, which means the only way to access the object is
    through instance
 */
ServerLink &ServerLink::getInstance()
{
	static ServerLink link;
	return link;
}

// Constructor creates the pimpl
ServerLink::ServerLink() : pimpl(std::make_unique<Pimpl>()) {}

ServerLink::~ServerLink() = default;

ServerLinkId ServerLink::serverLinkAdd(ServerLinkParams p)
{
	return pimpl->add(std::move(p));
}

void ServerLink::serverLinkUpdate(ServerLinkId id, ServerLinkParams p)
{
	pimpl->update(id, std::move(p));
}

void ServerLink::serverLinkRemove(ServerLinkId id)
{
	pimpl->remove(id);
}

void ServerLink::serverLinkSetActive(ServerLinkId id, bool active)
{
	pimpl->setActive(id, active);
}

void ServerLink::serverLinkEnqueue(std::string json)
{
	pimpl->enqueue(std::move(json));
}

void ServerLink::serverLinkApplyStreamGate()
{
	pimpl->applyStreamGate();
}

void ServerLink::setIsStreaming(bool live)
{
	pimpl->setIsStreaming(live);
}

bool ServerLink::serverLinkIsStreaming()
{
	return pimpl->isStreaming();
}

std::string ServerLink::serverLinkSessionKey()
{
	return pimpl->sessionKey();
}

size_t ServerLink::serverLinkActiveCount()
{
	return pimpl->activeCount();
}
