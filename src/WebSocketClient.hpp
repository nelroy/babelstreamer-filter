// SPDX-FileCopyrightText: 2026 Kamayu B.V. <https://www.kamayu.nl>
// SPDX-License-Identifier: GPL-2.0-or-later

//=======================================================================
//
//  WebSocketClient.hpp
//
//  Created: 20 sep 2026
//
//  Author: N.H. Elroy with support from Claude Code (Sonnet)
//
///  Abstract: Minimal RFC 6455 WebSocket client (text-frame send only).
//            BSD sockets (+ Winsock on Windows) for the transport;
//            mbedTLS for wss:// — the same TLS library OBS's own
//            obs-outputs/librtmp.c already uses for RTMPS, and already
//            bundled in the OBS deps this plugin builds against.
//
//=======================================================================

#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
static constexpr socket_t INVALID_SOCK = INVALID_SOCKET;
#else
using socket_t = int;
static constexpr socket_t INVALID_SOCK = -1;
#endif

/**
    Main websocket client - create one of these to connect to the server. There is
    only one link per OBS so the class is made non-copiable
 */
class WebSocketClient {
public:
	WebSocketClient();
	~WebSocketClient();

	// make non-copyable
	WebSocketClient(const WebSocketClient &) = delete;
	WebSocketClient &operator=(const WebSocketClient &) = delete;

	/**
        Connect to a host path. This supports both ws:// and wss:// as we need non-secure versions
        for testing. TLS does full certificate-chain + hostname verification against the OS trust store
        (MBEDTLS\_SSL\_VERIFY\_REQUIRED) — a bad, expired, self-signed, or wrong-host certificate
        fails the connection outright rather than silently encrypting to an unverified peer.
     
        The TCP connect, TLS handshake and the HTTP upgrade handshake all abandon after
        timeoutMs, to avoid blocking until the system times out.
     
        @param host                    host-name, could be a DNS resolvable host or a IP address
        @param port                     port number for connecting to host
        @param path                    path to access on host
        @param shouldUseTls           when true use secure sockets (wss://)
        @param extraHeaders     a list of extra headers to add to the connect request
     
        @returns true on success
     */
	bool connect(const std::string &host, uint16_t port, const std::string &path = "/", int timeoutMs = 5000,
		     bool shouldUseTls = false,
		     const std::vector<std::pair<std::string, std::string>> &extraHeaders = {});

	/**
        Send a UTF-8 text frame. Thread-safe.
     
        @param text    text to send
    
        @returns false failed or not connected, true otherwise
    */
	bool sendText(const std::string &text);

	/**
        Read one inbound text frame into waiting up to _timeoutMs_  before failng.
        Normally called from the same thread as connect() (the WhisperWrapper
        worker thread) — but safe to call concurrently with disconnect() from
        another thread too, since both hold ioMutex for their full duration;

        @param text    buffer to receive text
     
        @returns true if ok, false if not connected or timed out. Buffer undefined on failure
     */

	bool receiveText(std::string &out, int timeoutMs = 2000);

	/**
        Disconnect from any connection. Safe to call from any thread
     */
	void disconnect();

	/**
        Check if connection active
     
        @returns true if connected to a host
     */
	bool isConnected() const { return sockConnected.load(); }

	/**
        Optional callback to signal when the connection drops unexpectedly
     */
	std::function<void()> onDisconnect;

private:
	// This performs the actual connection handshake
	bool performHandshake(const std::string &host, const std::string &path,
			      const std::vector<std::pair<std::string, std::string>> &extraHeaders);

	// Receive from socket. Doesn't lock so caller must ensure that there is  a lock
	bool recvAll(char *buf, size_t len);

	/*
        Normalized low-level I/O — every send()/recv() call should use these
        instead of the socket/ssl system calls. Normalised return codes are:
     
        > 0  bytes transferred
        0    orderly close by the peer
        -1   timeout (SO_RCVTIMEO elapsed) — benign, caller must NOT mark
                the connection dead
        -2   a real error — caller should mark the connection dead
     */
	int ioSend(const char *buf, int len);
	int ioRecv(char *buf, int len);
	bool tlsHandshake(const std::string &host, int timeoutMs);
	void tlsTeardown();

	/*
        mbedTLS BIO callbacks — thin wrappers around the plain socket that
        reuse this class's own cross-platform timeout detection (matching
        recvAll's) rather than trusting mbedtls_net_send/recv's own mapping,
        which has historically had platform-specific quirks around
        WSAETIMEDOUT on Windows.
     */
	static int biosSend(void *ctx, const unsigned char *buf, size_t len);
	static int biosRecv(void *ctx, unsigned char *buf, size_t len);

	socket_t globalSocket = INVALID_SOCK;
	std::atomic<bool> sockConnected{false};

	// Mutex to serialise all socket actions
	mutable std::mutex ioMutex;

	// TLS (wss://) 
	bool useTls = false;
	bool tlsInited = false; // whether the mbedtls_*_init() calls below have
				// run (so tlsTeardown() knows what to free)
	mbedtls_ssl_context sslContext;
	mbedtls_ssl_config sslConfig;
	mbedtls_x509_crt certificateContext;
	mbedtls_ctr_drbg_context ctrDrbgContext;
	mbedtls_entropy_context entropyContext;
};
