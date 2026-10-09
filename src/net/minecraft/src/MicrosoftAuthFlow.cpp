#include "MicrosoftAuthFlow.h"

#include "AuthJson.h"
#include "MicrosoftAccount.h"
#include "PostHttp.h"

#include "platform/Log.h"
#include "platform/PlatformAuth.h"

#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
constexpr const char *kDeviceCodeUrl =
	"https://login.microsoftonline.com/consumers/oauth2/v2.0/devicecode";
constexpr const char *kTokenUrl =
	"https://login.microsoftonline.com/consumers/oauth2/v2.0/token";

std::string formBody(const std::vector<std::pair<std::string, std::string>> &fields)
{
	std::string body;
	bool first = true;
	for (const auto &field : fields)
	{
		if (!first)
			body += '&';
		first = false;
		body += PostHttp::urlEncode(field.first) + '=' + PostHttp::urlEncode(field.second);
	}
	return body;
}
} // namespace

MicrosoftAuthFlow::~MicrosoftAuthFlow()
{
	cancel();
	if (worker.joinable() && !worker.isCurrent())
		worker.join();
}

void MicrosoftAuthFlow::start()
{
	// 64 KiB: the TLS/HTTP work lives in the platform backend, but the JSON
	// chain and the endpoint strings still run on this stack.
	if (!worker.start(&MicrosoftAuthFlow::threadEntry, this, 64 * 1024, 64))
		MC_LOG_ERROR("auth", "Could not create the login worker thread\n");
}

void MicrosoftAuthFlow::cancel()
{
	cancelled.store(true);
}

bool MicrosoftAuthFlow::takeSnapshot(Snapshot &out)
{
	std::lock_guard<PlatformMutex> guard(lock);
	if (!snapshotFresh)
		return false;
	out = snapshot;
	snapshotFresh = false;
	return true;
}

void *MicrosoftAuthFlow::threadEntry(void *argument)
{
	static_cast<MicrosoftAuthFlow *>(argument)->run();
	return nullptr;
}

void MicrosoftAuthFlow::publish(const Snapshot &next)
{
	std::lock_guard<PlatformMutex> guard(lock);
	snapshot = next;
	snapshotFresh = true;
}

bool MicrosoftAuthFlow::waitCancellable(int_t seconds)
{
	for (int_t waited = 0; waited < seconds * 4; waited++)
	{
		if (cancelled.load())
			return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
	}
	return !cancelled.load();
}

void MicrosoftAuthFlow::run()
{
	Snapshot snap;
	publish(snap);

	const auto fail = [this, &snap](const std::string &reason)
	{
		snap.state = State::Failed;
		snap.error = reason;
		snap.finished = true;
		publish(snap);
	};

	const std::string clientId = MicrosoftAccounts::clientId();
	if (clientId.empty())
	{
		fail("This build was configured without a Microsoft application id (OPTICRAFT_MSA_CLIENT_ID)");
		return;
	}

	// 1. Ask for a device code (RFC 8628).
	PlatformAuth::HttpResponse response;
	PlatformAuth::HttpRequest request;
	request.url = kDeviceCodeUrl;
	request.method = "POST";
	request.headers = {{"Content-Type", "application/x-www-form-urlencoded"},
	                   {"Accept", "application/json"}};
	request.body = formBody({{"client_id", clientId},
	                         {"scope", "XboxLive.SignIn XboxLive.offline_access"}});
	if (!PlatformAuth::httpsRequest(request, response))
	{
		fail("Microsoft's login service could not be reached");
		return;
	}
	AuthJson::Value device;
	if (!AuthJson::parse(response.body, device))
	{
		fail("Microsoft's login service sent an unreadable answer");
		return;
	}
	const AuthJson::Value *deviceCodeMember = device.member("device_code");
	const AuthJson::Value *userCodeMember = device.member("user_code");
	const AuthJson::Value *uriMember = device.member("verification_uri");
	if (deviceCodeMember == nullptr || userCodeMember == nullptr || uriMember == nullptr ||
	    deviceCodeMember->textOr("").empty() || userCodeMember->textOr("").empty() ||
	    uriMember->textOr("").empty())
	{
		const AuthJson::Value *error = device.member("error");
		fail(error != nullptr
		         ? error->textOr("Microsoft refused to start the sign-in")
		         : "Microsoft's login service sent an incomplete answer");
		return;
	}
	long long expiresInSeconds = 900;
	if (const AuthJson::Value *expires = device.member("expires_in"))
		expires->numberAsInteger(expiresInSeconds);
	int_t intervalSeconds = 5;
	if (const AuthJson::Value *interval = device.member("interval"))
	{
		long long value = 5;
		if (interval->numberAsInteger(value) && value > 0)
			intervalSeconds = static_cast<int_t>(value);
	}

	snap.state = State::WaitingForUser;
	snap.userCode = userCodeMember->textOr("");
	snap.verificationUrl = uriMember->textOr("");
	publish(snap);

	// 2. Poll until the user finishes on another device (RFC 8628 pacing:
	// slow_down raises the interval; a stall must not own the worker).
	const std::string deviceCode = deviceCodeMember->textOr("");
	const auto deadline = std::chrono::steady_clock::now() +
	                      std::chrono::seconds(expiresInSeconds > 0 ? expiresInSeconds : 900);
	std::string msaAccessToken;
	std::string msaRefreshToken;
	for (;;)
	{
		if (cancelled.load())
			return;
		if (std::chrono::steady_clock::now() >= deadline)
		{
			fail("The sign-in code expired before the login finished");
			return;
		}

		request.url = kTokenUrl;
		request.body = formBody({{"client_id", clientId},
		                         {"grant_type", "urn:ietf:params:oauth:grant-type:device_code"},
		                         {"device_code", deviceCode}});
		if (!PlatformAuth::httpsRequest(request, response))
		{
			// A stall must not own the worker, but neither should a single
			// dropped packet scrap the whole login: back off one interval
			// and poll again (the deadline above still bounds the loop),
			// like PrismLauncher's device-code step retrying a failed poll
			// instead of failing the flow.
			if (!waitCancellable(intervalSeconds))
				return;
			continue;
		}
		AuthJson::Value token;
		if (!AuthJson::parse(response.body, token))
		{
			fail("Microsoft's login service sent an unreadable answer");
			return;
		}
		if (const AuthJson::Value *error = token.member("error"))
		{
			const std::string code = error->textOr("");
			if (code == "authorization_pending")
			{
				if (!waitCancellable(intervalSeconds))
					return;
				continue;
			}
			if (code == "slow_down")
			{
				// RFC 8628 section 3.5: the interval grows AND the client
				// still waits it out -- polling immediately would hammer
				// the endpoint every round-trip.
				intervalSeconds += 5;
				if (!waitCancellable(intervalSeconds))
					return;
				continue;
			}
			if (code == "expired_token")
			{
				fail("The sign-in code expired before the login finished");
				return;
			}
			const AuthJson::Value *description = token.member("error_description");
			fail(description != nullptr ? description->textOr(code) : code);
			return;
		}
		const AuthJson::Value *accessTokenMember = token.member("access_token");
		if (accessTokenMember == nullptr || accessTokenMember->textOr("").empty())
		{
			fail("Microsoft's login service sent an incomplete answer");
			return;
		}
		msaAccessToken = accessTokenMember->textOr("");
		if (const AuthJson::Value *refresh = token.member("refresh_token"))
			msaRefreshToken = refresh->textOr("");
		break;
	}

	// 3. The shared Xbox -> XSTS -> Minecraft chain.
	snap.state = State::ExchangingTokens;
	publish(snap);
	MicrosoftAccount account;
	account.clientId = clientId;
	if (!MicrosoftAccounts::exchangeMsaAccessToken(msaAccessToken, clientId, account, snap.error))
	{
		fail(snap.error);
		return;
	}
	account.msaRefreshToken = msaRefreshToken;
	MicrosoftAccounts::save(account);

	snap.state = State::Done;
	snap.finished = true;
	publish(snap);
}
