// SPDX-FileCopyrightText: 2026 Kamayu B.V. <https://www.kamayu.nl>
// SPDX-License-Identifier: GPL-2.0-or-later

//=======================================================================
//
//  WebSocketClient.cpp
//
//  Created: 20 sep 2026
//
//  Author: N.H. Elroy with support from Claude Code (Sonnet)
//
//=======================================================================

#include "WebSocketClient.hpp"
#include <obs-module.h>

#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <random>
#include <sstream>
#include <stdexcept>
#include <vector>

// Base64 encode (used for Sec-WebSocket-Key)
#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#define closesocket close
#endif

/*
   mbedTLS has no notion of an OS trust store the way OpenSSL's
   SSL_CTX_set_default_verify_paths() does - it just verifies against
   whatever's in the mbedtls_x509_crt chain it's given. Mirrors
   obs-studio/plugins/obs-outputs/librtmp/rtmp.c's RTMP_TLS_LoadCerts: same
   library, same problem, already solved there for every platform OBS itself
   ships on.
 */
#if defined(_WIN32)
#include <wincrypt.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#endif

static void loadSystemRootCerts(mbedtls_x509_crt &chain)
{
#if defined(_WIN32)
	HCERTSTORE hCertStore = CertOpenSystemStoreW((HCRYPTPROV_LEGACY)0, L"ROOT");
	if (!hCertStore) {
		blog(LOG_WARNING, "[babelstreamer-filter] TLS: CertOpenSystemStore failed; "
				  "wss:// will likely fail certificate verification");
		return;
	}
	PCCERT_CONTEXT certCtx = nullptr;
	while ((certCtx = CertEnumCertificatesInStore(hCertStore, certCtx)) != nullptr) {
		mbedtls_x509_crt_parse_der(&chain, (const unsigned char *)certCtx->pbCertEncoded,
					   certCtx->cbCertEncoded);
	}
	CertCloseStore(hCertStore, 0);
#elif defined(__APPLE__)
	CFArrayRef anchors = nullptr;
	if (SecTrustCopyAnchorCertificates(&anchors) != errSecSuccess) {
		blog(LOG_WARNING, "[babelstreamer-filter] TLS: SecTrustCopyAnchorCertificates "
				  "failed; wss:// will likely fail certificate verification");
		return;
	}
	for (CFIndex i = 0; i < CFArrayGetCount(anchors); i++) {
		auto cert = (SecCertificateRef)CFArrayGetValueAtIndex(anchors, i);
		CFDataRef der = SecCertificateCopyData(cert);
		const UInt8 *data = CFDataGetBytePtr(der);
		CFIndex length = CFDataGetLength(der);
		if (data && length > 0)
			mbedtls_x509_crt_parse_der(&chain, data, (size_t)length);
		CFRelease(der);
	}
	CFRelease(anchors);
#else
	// Linux/BSD: standard system CA bundle locations.
	if (mbedtls_x509_crt_parse_path(&chain, "/etc/ssl/certs") != 0)
		mbedtls_x509_crt_parse_file(&chain, "/etc/ssl/cert.pem");
#endif
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64Encode(const uint8_t *in, size_t len)
{
	std::string out;
	out.reserve(((len + 2) / 3) * 4);
	for (size_t i = 0; i < len; i += 3) {
		uint32_t v = (uint32_t)in[i] << 16;
		if (i + 1 < len)
			v |= (uint32_t)in[i + 1] << 8;
		if (i + 2 < len)
			v |= (uint32_t)in[i + 2];
		out += B64[(v >> 18) & 0x3f];
		out += B64[(v >> 12) & 0x3f];
		out += (i + 1 < len) ? B64[(v >> 6) & 0x3f] : '=';
		out += (i + 2 < len) ? B64[v & 0x3f] : '=';
	}
	return out;
}

// Generate a random 16-byte nonce for Sec-WebSocket-Key
static std::string makeWsKey()
{
	std::array<uint8_t, 16> nonce{};
	std::mt19937 rng(std::random_device{}());
	std::uniform_int_distribution<int> dist(0, 255);
	for (auto &b : nonce)
		b = static_cast<uint8_t>(dist(rng));
	return base64Encode(nonce.data(), nonce.size());
}

WebSocketClient::WebSocketClient()
{
#ifdef _WIN32
	WSADATA wd;
	WSAStartup(MAKEWORD(2, 2), &wd);
#endif
}

WebSocketClient::~WebSocketClient()
{
	disconnect();
#ifdef _WIN32
	WSACleanup();
#endif
}

/*
    ::connect() with a bounded wait: non-blocking connect, then poll for
    writability, then check SO_ERROR to learn the actual result. The socket is
    restored to blocking mode afterwards - everything else in this class uses
    blocking I/O with per-call SO_RCVTIMEO timeouts. poll()/WSAPoll() rather
    than select(): inside a process like OBS, descriptor numbers can exceed
    FD_SETSIZE (1024), which is undefined behavior for select()'s fd_set.
 */
static bool connectWithTimeout(socket_t s, const sockaddr *addr, socklen_t addrlen, int timeoutMs)
{
#ifdef _WIN32
	u_long nonblock = 1;
	ioctlsocket(s, FIONBIO, &nonblock);
#else
	const int flags = fcntl(s, F_GETFL, 0);
	fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif

	bool ok = false;
	if (::connect(s, addr, (int)addrlen) == 0) {
		ok = true; // completed immediately (typical for localhost)
	} else {
#ifdef _WIN32
		const bool inProgress = (WSAGetLastError() == WSAEWOULDBLOCK);
#else
		const bool inProgress = (errno == EINPROGRESS);
#endif
		if (inProgress) {
#ifdef _WIN32
			WSAPOLLFD pfd{};
			pfd.fd = s;
			pfd.events = POLLOUT;
			const int rc = WSAPoll(&pfd, 1, timeoutMs);
#else
			struct pollfd pfd{};
			pfd.fd = s;
			pfd.events = POLLOUT;
			const int rc = poll(&pfd, 1, timeoutMs);
#endif
			if (rc > 0 && (pfd.revents & POLLOUT)) {
				// Writable can also mean "connect failed" - SO_ERROR is
				// the authoritative result.
				int err = 0;
				socklen_t len = sizeof(err);
				getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&err), &len);
				ok = (err == 0);
			}
		}
	}

#ifdef _WIN32
	u_long block = 0;
	ioctlsocket(s, FIONBIO, &block);
#else
	fcntl(s, F_SETFL, flags);
#endif
	return ok;
}

/*
    Sets (or clears, with 0) the socket's blocking-receive timeout. Shared by
    connect() - which bounds the handshake read with it - and receiveText().
 */
static void setRecvTimeout(socket_t s, int timeoutMs)
{
#ifdef _WIN32
	DWORD tv = static_cast<DWORD>(timeoutMs);
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&tv), sizeof(tv));
#else
	struct timeval tv;
	tv.tv_sec = timeoutMs / 1000;
	tv.tv_usec = (timeoutMs % 1000) * 1000;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

/*  mbedTLS BIO callbacks
 
    webSocketClient is the owning WebSocketClient. Reuses this class's own cross-platform
    timeout detection (WSAETIMEDOUT/WSAEWOULDBLOCK on Windows, EAGAIN/EWOULDBLOCK
    elsewhere) instead of trusting mbedtls_net_send/recv's own mapping.
 */
int WebSocketClient::biosSend(void *webSocketClient, const unsigned char *buf, size_t len)
{
	auto *self = static_cast<WebSocketClient *>(webSocketClient);
	int n = send(self->globalSocket, reinterpret_cast<const char *>(buf), (int)len, 0);
	if (n >= 0)
		return n;
#ifdef _WIN32
	int err = WSAGetLastError();
	if (err == WSAEWOULDBLOCK || err == WSAETIMEDOUT)
		return MBEDTLS_ERR_SSL_WANT_WRITE;
#else
	if (errno == EAGAIN || errno == EWOULDBLOCK)
		return MBEDTLS_ERR_SSL_WANT_WRITE;
#endif
	return MBEDTLS_ERR_NET_SEND_FAILED;
}

int WebSocketClient::biosRecv(void *ctx, unsigned char *buf, size_t len)
{
	auto *self = static_cast<WebSocketClient *>(ctx);
	int n = recv(self->globalSocket, reinterpret_cast<char *>(buf), (int)len, 0);
	if (n > 0)
		return n;
	if (n == 0)
		return 0; // orderly close - mbedTLS expects 0 to mean EOF, same as POSIX recv()
#ifdef _WIN32
	int err = WSAGetLastError();
	if (err == WSAEWOULDBLOCK || err == WSAETIMEDOUT)
		return MBEDTLS_ERR_SSL_WANT_READ;
#else
	if (errno == EAGAIN || errno == EWOULDBLOCK)
		return MBEDTLS_ERR_SSL_WANT_READ;
#endif
	return MBEDTLS_ERR_NET_RECV_FAILED;
}

/*
    Normalised I/O routines. See .hpp for explanation
 */
namespace {
constexpr int IO_TIMEOUT = -1;
constexpr int IO_ERROR = -2;
} // namespace

int WebSocketClient::ioSend(const char *buf, int len)
{
	if (!useTls) {
		int n = send(globalSocket, buf, len, 0);
		if (n >= 0)
			return n;
#ifdef _WIN32
		int err = WSAGetLastError();
		if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK)
			return IO_TIMEOUT;
#else
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return IO_TIMEOUT;
#endif
		return IO_ERROR;
	}

	int ret = mbedtls_ssl_write(&sslContext, reinterpret_cast<const unsigned char *>(buf), (size_t)len);
	if (ret >= 0)
		return ret;
	if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
		return IO_TIMEOUT;
	return IO_ERROR;
}

int WebSocketClient::ioRecv(char *buf, int len)
{
	if (!useTls) {
		int n = recv(globalSocket, buf, len, 0);
		if (n > 0)
			return n;
		if (n == 0)
			return 0;
#ifdef _WIN32
		int err = WSAGetLastError();
		if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK)
			return IO_TIMEOUT;
#else
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return IO_TIMEOUT;
#endif
		return IO_ERROR;
	}

	int ret = mbedtls_ssl_read(&sslContext, reinterpret_cast<unsigned char *>(buf), (size_t)len);
	if (ret > 0)
		return ret;
	if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
		return 0;
	if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
		return IO_TIMEOUT;
	return IO_ERROR;
}

/*
    TLS handshake / teardown. Sets up an mbedTLS client context against the
    system's own root certificate store, verifies the peer's full chain and
    hostname (see MBEDTLS_SSL_VERIFY_REQUIRED below), and drives the
    handshake against a wall-clock deadline so a peer that accepts the TCP
    connection but goes silent fails bounded rather than hanging forever.
*/
bool WebSocketClient::tlsHandshake(const std::string &host, int timeoutMs)
{
	mbedtls_ssl_init(&sslContext);
	mbedtls_ssl_config_init(&sslConfig);
	mbedtls_x509_crt_init(&certificateContext);
	mbedtls_ctr_drbg_init(&ctrDrbgContext);
	mbedtls_entropy_init(&entropyContext);
	tlsInited = true;

	const char *pers = "babelstreamer-ws";
	if (mbedtls_ctr_drbg_seed(&ctrDrbgContext, mbedtls_entropy_func, &entropyContext,
				  reinterpret_cast<const unsigned char *>(pers), strlen(pers)) != 0) {
		blog(LOG_WARNING, "[babelstreamer-filter] TLS: RNG seed failed");
		return false;
	}

	if (mbedtls_ssl_config_defaults(&sslConfig, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
					MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
		blog(LOG_WARNING, "[babelstreamer-filter] TLS: config defaults failed");
		return false;
	}
	mbedtls_ssl_conf_rng(&sslConfig, mbedtls_ctr_drbg_random, &ctrDrbgContext);

	loadSystemRootCerts(certificateContext);
	mbedtls_ssl_conf_ca_chain(&sslConfig, &certificateContext, nullptr);

	// Full chain + hostname verification too
	mbedtls_ssl_conf_authmode(&sslConfig, MBEDTLS_SSL_VERIFY_REQUIRED);

	if (mbedtls_ssl_setup(&sslContext, &sslConfig) != 0) {
		blog(LOG_WARNING, "[babelstreamer-filter] TLS: ssl_setup failed");
		return false;
	}

	// SNI + the name mbedTLS checks the leaf certificate's SAN/CN against.
	if (mbedtls_ssl_set_hostname(&sslContext, host.c_str()) != 0) {
		blog(LOG_WARNING, "[babelstreamer-filter] TLS: set_hostname failed");
		return false;
	}

	mbedtls_ssl_set_bio(&sslContext, this, &WebSocketClient::biosSend, &WebSocketClient::biosRecv, nullptr);

	// bailout in case you get into a retry loop
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	int ret;
	do {
		ret = mbedtls_ssl_handshake(&sslContext);
	} while ((ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) &&
		 std::chrono::steady_clock::now() < deadline);

	if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
		blog(LOG_WARNING, "[babelstreamer-filter] TLS handshake timed out for %s", host.c_str());
		return false;
	}

	if (ret != 0) {
		char errbuf[256];
		mbedtls_strerror(ret, errbuf, sizeof(errbuf));
		uint32_t verifyResult = mbedtls_ssl_get_verify_result(&sslContext);
		if (verifyResult != 0 && verifyResult != 0xFFFFFFFFu) {
			char vbuf[256];
			mbedtls_x509_crt_verify_info(vbuf, sizeof(vbuf), "", verifyResult);
			blog(LOG_WARNING, "[babelstreamer-filter] TLS handshake failed for %s: %s (cert: %s)",
			     host.c_str(), errbuf, vbuf);
		} else {
			blog(LOG_WARNING, "[babelstreamer-filter] TLS handshake failed for %s: %s", host.c_str(),
			     errbuf);
		}
		return false;
	}

	blog(LOG_INFO, "[babelstreamer-filter] TLS handshake OK (%s)", mbedtls_ssl_get_ciphersuite(&sslContext));
	return true;
}

void WebSocketClient::tlsTeardown()
{
	if (!tlsInited)
		return;
	mbedtls_ssl_free(&sslContext);
	mbedtls_ssl_config_free(&sslConfig);
	mbedtls_x509_crt_free(&certificateContext);
	mbedtls_ctr_drbg_free(&ctrDrbgContext);
	mbedtls_entropy_free(&entropyContext);
	tlsInited = false;
}

/*
    Connection handlong for the socket. Main steps are:
    1) resolve host name
    2) try a TCP connect
    3) try the TLS handshake to upgrade to a secure socket, if needed
    4) report back
 */
bool WebSocketClient::connect(const std::string &host, uint16_t port, const std::string &path, int timeoutMs,
			      bool shouldUseTls, const std::vector<std::pair<std::string, std::string>> &extraHeaders)
{
	disconnect();

	// Resolve host
	addrinfo hints{};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	addrinfo *res = nullptr;
	std::string portStr = std::to_string(port);
	if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0) {
		blog(LOG_WARNING, "[babelstreamer-filter] DNS resolution failed for %s", host.c_str());
		return false;
	}

	// Try all the resolved IPs until one works, or everything fails
	socket_t mySocket = INVALID_SOCK;
	for (addrinfo *ai = res; ai; ai = ai->ai_next) {
		mySocket = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (mySocket == INVALID_SOCK)
			continue;
		if (connectWithTimeout(mySocket, ai->ai_addr, (socklen_t)ai->ai_addrlen, timeoutMs))
			break;
		closesocket(mySocket);
		mySocket = INVALID_SOCK;
	}
	freeaddrinfo(res);

	if (mySocket == INVALID_SOCK) {
		blog(LOG_WARNING, "[babelstreamer-filter] TCP connect failed to %s:%u", host.c_str(), port);
		return false;
	}

	// set TCP_NODELAY for lower latency
	int flag = 1;
	setsockopt(mySocket, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(flag));

	// Locked from here on: this is the first point globalSocket becomes visible to
	// a concurrent disconnect()
	std::lock_guard<std::mutex> lock(ioMutex);
	globalSocket = mySocket;
	useTls = shouldUseTls;

	// Bounds the TLS handshake (if any) and the HTTP upgrade handshake: a
	// host that accepts the TCP connection but never answers would otherwise
	// hang forever on a recv() somewhere below.
	setRecvTimeout(globalSocket, timeoutMs);

	if (useTls && !tlsHandshake(host, timeoutMs)) {
		tlsTeardown();
		useTls = false;
		closesocket(globalSocket);
		globalSocket = INVALID_SOCK;
		return false;
	}

	// Do the actual handshake to upgrade to ws:// or wss://
	const bool handshakeOk = performHandshake(host, path, extraHeaders);
	if (globalSocket != INVALID_SOCK)
		setRecvTimeout(globalSocket, 0); // back to no-timeout; receiveText() sets its own

	if (!handshakeOk) {
		if (useTls) {
			mbedtls_ssl_close_notify(&sslContext);
			tlsTeardown();
			useTls = false;
		}
		closesocket(globalSocket);
		globalSocket = INVALID_SOCK;
		return false;
	}

	sockConnected.store(true);
	blog(LOG_INFO, "[babelstreamer-filter] Connected to %s://%s:%u%s", useTls ? "wss" : "ws", host.c_str(), port,
	     path.c_str());
	return true;
}

/*
    WebSocket HTTP upgrade handshake. Do a HTTP request to upgrade to a websocket.
    Secure socket already determined in the link setup above.
 */
bool WebSocketClient::performHandshake(const std::string &host, const std::string &path,
				       const std::vector<std::pair<std::string, std::string>> &extraHeaders)
{
	std::string key = makeWsKey();

	std::ostringstream req;
	req << "GET " << path << " HTTP/1.1\r\n"
	    << "Host: " << host << "\r\n"
	    << "Upgrade: websocket\r\n"
	    << "Connection: Upgrade\r\n"
	    << "Sec-WebSocket-Key: " << key << "\r\n"
	    << "Sec-WebSocket-Version: 13\r\n";
	for (const auto &[name, value] : extraHeaders)
		req << name << ": " << value << "\r\n";
	req << "\r\n";

	std::string reqStr = req.str();
	if (ioSend(reqStr.c_str(), (int)reqStr.size()) < 0) {
		blog(LOG_WARNING, "[babelstreamer-filter] Failed to send HTTP upgrade");
		return false;
	}

	// Read the response headers (look for end of headers \r\n\r\n)
	std::string resp;
	resp.reserve(512);
	char ch;
	while (resp.size() < 4096) {
		if (ioRecv(&ch, 1) != 1) {
			blog(LOG_WARNING, "[babelstreamer-filter] Connection closed during handshake");
			return false;
		}
		resp += ch;
		if (resp.size() >= 4 && resp.compare(resp.size() - 4, 4, "\r\n\r\n") == 0)
			break;
	}

	if (resp.find("101") == std::string::npos) {
		blog(LOG_WARNING, "[babelstreamer-filter] Server did not return 101 Switching Protocols");
		return false;
	}
	return true;
}

/*
    Encodes a UTF-8 string as a masked WebSocket text frame (RFC 6455 §5.2). For
    details of the encoding see the RFC.
    
 */
bool WebSocketClient::sendText(const std::string &text)
{
	if (!sockConnected.load())
		return false;
	std::lock_guard<std::mutex> lock(ioMutex);
	if (!sockConnected.load())
		return false; // re-check: disconnect() may have won the race for the lock

	const uint8_t *payload = reinterpret_cast<const uint8_t *>(text.data());
	size_t payloadLen = text.size();

	// Frame header buffer (2 + up to 8 length bytes + 4 mask bytes = 14 max)
	std::vector<uint8_t> frame;
	frame.reserve(14 + payloadLen);

	// Byte 0: FIN=1, opcode=0x1 (text)
	frame.push_back(0x81);

	// Byte 1: MASK=1 + payload length
	if (payloadLen < 126) {
		frame.push_back(0x80 | static_cast<uint8_t>(payloadLen));
	} else if (payloadLen <= 0xFFFF) {
		frame.push_back(0x80 | 126);
		frame.push_back(static_cast<uint8_t>(payloadLen >> 8));
		frame.push_back(static_cast<uint8_t>(payloadLen & 0xFF));
	} else {
		frame.push_back(0x80 | 127);
		for (int i = 7; i >= 0; --i)
			frame.push_back(static_cast<uint8_t>((payloadLen >> (i * 8)) & 0xFF));
	}

	// 4-byte masking key (random)
	std::mt19937 rng(std::random_device{}());
	std::array<uint8_t, 4> mask{};
	std::uniform_int_distribution<int> dist(0, 255);
	for (auto &b : mask)
		b = static_cast<uint8_t>(dist(rng));
	frame.insert(frame.end(), mask.begin(), mask.end());

	// Masked payload
	for (size_t i = 0; i < payloadLen; ++i)
		frame.push_back(payload[i] ^ mask[i % 4]);

	// Send all at once
	const char *buf = reinterpret_cast<const char *>(frame.data());
	size_t left = frame.size();
	while (left > 0) {
		int sent = ioSend(buf, (int)left);
		if (sent <= 0) {
			blog(LOG_WARNING, "[babelstreamer-filter] send() failed, disconnecting");
			sockConnected.store(false);
			if (onDisconnect)
				onDisconnect();
			return false;
		}
		buf += sent;
		left -= static_cast<size_t>(sent);
	}
	return true;
}

/*
    Reads exactly len bytes from the socket, looping over short reads.
    A benign timeout (ioRecv() == IO_TIMEOUT) must return false WITHOUT
    flipping sockConnected, since callers that poll with a short timeout (e.g.
    WhisperFilter.cpp's drainPendingMessages, opportunistically checking for
    pushed server messages) would otherwise see the live connection marked
    dead on nearly every call.
 */
bool WebSocketClient::recvAll(char *buf, size_t len)
{
	size_t done = 0;
	while (done < len) {
		int n = ioRecv(buf + done, static_cast<int>(len - done));
		if (n == 0) {
			sockConnected.store(false); // orderly close by the peer
			return false;
		}
		if (n < 0) {
			if (n != -1 /* IO_TIMEOUT */)
				sockConnected.store(false); // a real error, not a timeout
			return false;
		}
		done += static_cast<size_t>(n);
	}
	return true;
}

/*
    Reads one server→client WebSocket frame with a receive timeout.
    Server frames are NOT masked (RFC 6455 §5.1).
 */
bool WebSocketClient::receiveText(std::string &out, int timeoutMs)
{
	if (!sockConnected.load())
		return false;

	// Lock the socket for the duration. Any disconnect will have to wait until this finishes
	std::lock_guard<std::mutex> lock(ioMutex);
	if (!sockConnected.load())
		return false; // re-check: disconnect() may have won the race for the lock

	// Set a receive timeout so we don't block forever
	setRecvTimeout(globalSocket, timeoutMs);
	auto resetTimeout = [&]() {
		setRecvTimeout(globalSocket, 0);
	};

	// Read the 2-byte frame header
	uint8_t header[2];
	if (!recvAll(reinterpret_cast<char *>(header), 2)) {
		resetTimeout();
		return false;
	}

	const uint8_t opcode = header[0] & 0x0F;
	const bool masked = (header[1] & 0x80) != 0;
	uint64_t paylen = header[1] & 0x7F;

	// Extended payload length
	if (paylen == 126) {
		uint8_t ext[2];
		if (!recvAll(reinterpret_cast<char *>(ext), 2)) {
			resetTimeout();
			return false;
		}
		paylen = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
	} else if (paylen == 127) {
		uint8_t ext[8];
		if (!recvAll(reinterpret_cast<char *>(ext), 8)) {
			resetTimeout();
			return false;
		}
		paylen = 0;
		for (int i = 0; i < 8; ++i)
			paylen = (paylen << 8) | ext[i];
	}

	// Optional mask (servers shouldn't send masked frames, but handle it)
	uint8_t mask[4] = {0, 0, 0, 0};
	if (masked) {
		if (!recvAll(reinterpret_cast<char *>(mask), 4)) {
			resetTimeout();
			return false;
		}
	}

	// Sanity cap - session key frame will be tiny
	if (paylen > 65536) {
		resetTimeout();
		return false;
	}

	std::string payload(static_cast<size_t>(paylen), '\0');
	if (paylen > 0 && !recvAll(payload.data(), static_cast<size_t>(paylen))) {
		resetTimeout();
		return false;
	}
	if (masked) {
		for (size_t i = 0; i < static_cast<size_t>(paylen); ++i)
			payload[i] ^= mask[i % 4];
	}

	resetTimeout();

	if (opcode != 0x1)
		return false; // not a text frame

	out = std::move(payload);
	return true;
}

/*
    Disconnect the socket for graceful teardown. The socket needs to
    be locked for the duraton of the teardown
 */
void WebSocketClient::disconnect()
{
	std::lock_guard<std::mutex> lock(ioMutex);
	if (globalSocket == INVALID_SOCK) {
		if (useTls) {
			tlsTeardown();
			useTls = false;
		}
		return;
	}

	/*
        Send a WebSocket close frame (opcode 0x8, no payload) - via ioSend so
        it goes out encrypted when useTls is set, then the TLS-level
        close_notify, then the raw socket.
     */
	if (sockConnected.load()) {
		const uint8_t closeFrame[] = {0x88, 0x80, 0x00, 0x00, 0x00, 0x00};
		ioSend(reinterpret_cast<const char *>(closeFrame), sizeof(closeFrame));
	}

	if (useTls) {
		mbedtls_ssl_close_notify(&sslContext); // best-effort, ignore result
		tlsTeardown();
		useTls = false;
	}

	closesocket(globalSocket);
	globalSocket = INVALID_SOCK;
	sockConnected.store(false);
}
