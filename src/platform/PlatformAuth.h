#pragma once

#include <string>
#include <utility>
#include <vector>

// PlatformAuth -- the Microsoft/Mojang login *transport* family
// (cmake/SourceSelection.cmake selects exactly one AuthBackend_*.cpp).
//
// The account flow itself is common code -- src/net/minecraft/src/
// MicrosoftAuthFlow.cpp (device-code login worker) and MicrosoftAccount.cpp
// (token cache, refresh, session join) -- so only the TLS pipe is
// platform-specific:
//   PC    -> vendored Mbed TLS (external/mbedtls), always linked by the
//            desktop target
//   3DS   -> 3ds-curl (libcurl + Mbed TLS portlibs, the same stack the QR
//            downloader links) with chain verification ON against the
//            Mozilla CA bundle staged to <app>/cacert.pem by 3ds-data
//   Wii/PS2 -> no TLS stack in-tree; login is simply unavailable
namespace PlatformAuth
{
	struct HttpRequest
	{
		std::string url;    // https:// only
		std::string method; // "GET" or "POST"
		std::vector<std::pair<std::string, std::string>> headers;
		std::string body;
	};

	struct HttpResponse
	{
		int status = 0;
		std::string body;
	};

	// True when this build can talk to the Microsoft login endpoints at all.
	bool microsoftLoginSupported();

	// One HTTPS round-trip. Returns false only for transport-level failures
	// (DNS, TCP, TLS, truncated body); an HTTP 4xx carrying the server's own
	// JSON error counts as a *successful* exchange so the flow can surface
	// the server's error codes (XErr etc.). Blocking -- call from a worker.
	bool httpsRequest(const HttpRequest &request, HttpResponse &response);
}
