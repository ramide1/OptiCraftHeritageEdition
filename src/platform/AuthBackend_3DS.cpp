// AuthBackend_3DS.cpp -- Microsoft/Mojang login transport for the 3DS.
//
// 3ds-curl (libcurl + Mbed TLS portlibs) -- the same stack the QR downloader
// links (see src/3ds/qr/DsHttpDownload.cpp for why curl and not the native
// httpc service) -- but with certificate verification ON: unlike the QR
// payload, which is a public file, this channel carries account tokens, so
// the chain is verified against the Mozilla CA bundle staged to
// <app>/cacert.pem by the 3ds-data target. A missing bundle fails the
// request with a log line instead of silently disabling verification.
//
// Requests run on the login worker thread (blocking curl_easy_perform), the
// way every other blocking network worker the game already runs on the
// console does (ThreadConnectToServer, NetworkManager's reader/writer). The
// QR downloader drives curl from the game loop instead, but its reason is a
// live progress bar, which a login handshake has no use for.

#include "platform/PlatformAuth.h"

#ifdef CTR_PLATFORM

// curl's multi.h declares fd_set-taking entry points but does not include
// the header itself (known 3ds portlibs quirk -- see DsHttpDownload.cpp);
// provide it first.
#include <sys/select.h>

#include <curl/curl.h>

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <mutex>
#include <string>

#include "platform/Log.h"
#include "platform/Resources.h"

#if defined(CTR_ENABLE_NETWORK)
#include "3ds/DsNetwork.h"
#else
#include <3ds.h>
#include <malloc.h>
#endif

namespace
{
constexpr long kConnectTimeoutSecs = 20;
constexpr long kLowSpeedLimit = 1;
constexpr long kLowSpeedTimeSecs = 30;

// Tokens and profiles are tiny; anything bigger is a protocol bug or a
// hostile host, and old3DS RAM is not the place to find out which.
constexpr std::size_t kMaxResponseBytes = 1024 * 1024;

std::once_flag curlOnce;
std::atomic<bool> curlReady{false};

#if !defined(CTR_ENABLE_NETWORK)
u32 *socSegment = nullptr;
#endif

bool ensureSockets()
{
#if defined(CTR_ENABLE_NETWORK)
	if (DsNetwork::isReady())
		return true;
	const std::string wifiError = DsNetwork::wifiPreflightError();
	if (!wifiError.empty())
	{
		MC_LOG_ERROR("auth", "3ds: login transport: %s\n", wifiError.c_str());
		return false;
	}
	return DsNetwork::initialize();
#else
	if (socSegment != nullptr)
		return true;
	socSegment = static_cast<u32 *>(memalign(0x1000, 0x100000));
	if (socSegment == nullptr || socInit(socSegment, 0x100000) != 0)
	{
		std::free(socSegment);
		socSegment = nullptr;
		return false;
	}
	return true;
#endif
}

std::size_t appendToBody(char *ptr, std::size_t size, std::size_t nmemb, void *userdata)
{
	std::string *body = static_cast<std::string *>(userdata);
	const std::size_t bytes = size * nmemb;
	if (body->size() + bytes > kMaxResponseBytes)
		return 0; // short write: curl aborts the transfer
	body->append(ptr, bytes);
	return bytes;
}
} // namespace

bool PlatformAuth::microsoftLoginSupported()
{
#if defined(CTR_ENABLE_NETWORK)
	return true;
#else
	return false;
#endif
}

bool PlatformAuth::httpsRequest(const HttpRequest &request, HttpResponse &response)
{
	response = HttpResponse();
	if (request.url.compare(0, 8, "https://") != 0)
		return false;
	if (!ensureSockets())
		return false;

	std::call_once(curlOnce, []
	{
		curlReady.store(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
	});
	if (!curlReady.load())
		return false;

	// There is no system cert store on the console: verification is only
	// possible against the staged Mozilla bundle. A missing bundle is a
	// deployment error, never a reason to fall back to VERIFY off.
	const std::string caPath = PlatformResources::resolveExisting("cacert.pem");
	if (caPath.empty())
	{
		MC_LOG_ERROR("auth",
		             "3ds: cacert.pem is missing from the app dir; re-run 'build 3ds.bat data' "
		             "(the 3ds-data target) and recopy the SD tree\n");
		return false;
	}

	CURL *easy = curl_easy_init();
	if (easy == nullptr)
		return false;

	std::string body;
	curl_slist *headers = nullptr;
	bool ok = false;

	for (const auto &header : request.headers)
	{
		curl_slist *appended = curl_slist_append(headers, (header.first + ": " + header.second).c_str());
		if (appended == nullptr)
			break;
		headers = appended;
	}

	curl_easy_setopt(easy, CURLOPT_URL, request.url.c_str());
	curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(easy, CURLOPT_USERAGENT, "OptiCraft-Heritage/3DS");
	curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L);
	curl_easy_setopt(easy, CURLOPT_CAINFO, caPath.c_str());
	curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSecs);
	curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, kLowSpeedLimit);
	curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, kLowSpeedTimeSecs);
	curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &appendToBody);
	curl_easy_setopt(easy, CURLOPT_WRITEDATA, &body);

	if (request.method == "POST")
	{
		// The JSON bodies never contain a NUL, and request.body outlives the
		// perform, so the borrowed-pointer POSTFIELDS is exact.
		curl_easy_setopt(easy, CURLOPT_POST, 1L);
		curl_easy_setopt(easy, CURLOPT_POSTFIELDS, request.body.c_str());
	}

	const CURLcode result = curl_easy_perform(easy);
	if (result == CURLE_OK)
	{
		long status = 0;
		if (curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status) == CURLE_OK)
		{
			response.status = static_cast<int>(status);
			response.body = std::move(body);
			ok = true;
		}
	}
	else
	{
		MC_LOG_ERROR("auth", "3ds: %s %s failed: %s\n",
		             request.method.c_str(), request.url.c_str(), curl_easy_strerror(result));
	}

	curl_easy_cleanup(easy);
	curl_slist_free_all(headers);
	return ok;
}

#else // !CTR_PLATFORM

// SourceSelection only ever builds this file for the 3DS target; the stub
// keeps it valid anywhere else, the same way DsHttpDownload.cpp does.
bool PlatformAuth::microsoftLoginSupported()
{
	return false;
}

bool PlatformAuth::httpsRequest(const HttpRequest &request, HttpResponse &response)
{
	(void)request;
	(void)response;
	return false;
}

#endif // CTR_PLATFORM
