#include "MicrosoftAccount.h"

#include "AuthJson.h"
#include "CompressedStreamTools.h"
#include "Minecraft.h"
#include "NBTTagCompound.h"
#include "PostHttp.h"
#include "Session.h"
#include "java/File.h"

#include "platform/Log.h"
#include "platform/PlatformAuth.h"
#include "platform/Storage.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <utility>
#include <vector>

#ifndef OPTICRAFT_MSA_CLIENT_ID
#define OPTICRAFT_MSA_CLIENT_ID ""
#endif

// The wire flow mirrors the documented Microsoft identity + Minecraft
// services chain every Java launcher speaks today (MS Learn, RFC 8628 device
// code grant, and the xboxlive/minecraftservices endpoints PrismLauncher,
// ViaProxy and the official launcher implement).
namespace
{
constexpr const char *kTokenUrl = "https://login.microsoftonline.com/consumers/oauth2/v2.0/token";
constexpr const char *kXboxUserUrl = "https://user.auth.xboxlive.com/user/authenticate";
constexpr const char *kXstsUrl = "https://xsts.auth.xboxlive.com/xsts/authorize";
constexpr const char *kMinecraftLoginUrl = "https://api.minecraftservices.com/authentication/login_with_xbox";
constexpr const char *kEntitlementsUrl = "https://api.minecraftservices.com/entitlements/mcstore";
constexpr const char *kProfileUrl = "https://api.minecraftservices.com/minecraft/profile";
constexpr const char *kSessionJoinUrl = "https://sessionserver.mojang.com/session/minecraft/join";

// Refresh five minutes before the announced expiry so an in-flight join
// never carries a token that dies mid-flight.
constexpr long_t kTokenSkewMs = 300000;

MicrosoftAccount gAccount;
bool gLoaded = false;

long_t nowMs()
{
	return static_cast<long_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string accountPath()
{
	File *dataDir = Minecraft::getMinecraftDir();
	if (dataDir == nullptr)
		return std::string();
	return PlatformStorage::join(dataDir->toString(), "accounts.nbt");
}

void loadFromDisk()
{
	gLoaded = true;
	gAccount = MicrosoftAccount();
	const std::string path = accountPath();
	if (path.empty() || !PlatformStorage::exists(path))
		return;
	try
	{
		std::vector<unsigned char> bytes;
		if (!PlatformStorage::readFile(path, bytes))
			return;
		std::vector<char> compressed(bytes.begin(), bytes.end());
		std::unique_ptr<NBTTagCompound> root(CompressedStreamTools::decompress(compressed));
		if (root == nullptr)
			return;
		gAccount.username = root->getString("username");
		gAccount.uuid = root->getString("uuid");
		gAccount.msaRefreshToken = root->getString("msaRefreshToken");
		gAccount.mcAccessToken = root->getString("mcAccessToken");
		gAccount.clientId = root->getString("clientId");
		gAccount.mcTokenExpiresMs = root->getLong("mcTokenExpiresMs");
	}
	catch (const std::exception &exception)
	{
		MC_LOG_WARN("auth", "Unable to read accounts.nbt: %s\n", exception.what());
		gAccount = MicrosoftAccount();
	}
}

using Headers = std::vector<std::pair<std::string, std::string>>;

bool performJson(const std::string &method, const std::string &url,
                 const Headers &headers, const std::string &body,
                 PlatformAuth::HttpResponse &outResponse)
{
	PlatformAuth::HttpRequest request;
	request.url = url;
	request.method = method;
	request.headers = headers;
	request.body = body;
	return PlatformAuth::httpsRequest(request, outResponse);
}

std::string formBody(const Headers &fields)
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

// Microsoft's XSTS refusal codes, documented with every launcher that
// implements the flow; the wording is our own.
std::string describeXstsError(long long xerr)
{
	switch (xerr)
	{
	case 2148916233LL:
		return "This Microsoft account has no Xbox profile. Buy and start Minecraft on it once at minecraft.net first.";
	case 2148916235LL:
		return "Xbox Live is not available in this account's country.";
	case 2148916236LL:
		return "This account must confirm its age at login.live.com first.";
	case 2148916237LL:
		return "This account reached its playtime limit and cannot sign in.";
	case 2148916238LL:
		return "This is a child account without a family setup. Configure it at account.microsoft.com first.";
	case 2148916227LL:
		return "This account was banned by Xbox and cannot sign in.";
	case 2148916229LL:
		return "This family-restricted account needs a guardian to allow online play first.";
	case 2148916234LL:
		return "This account must accept the Xbox terms of use first.";
	default:
		return "Microsoft refused to authorize this account (Xbox error " +
		       std::to_string(xerr) + ")";
	}
}
} // namespace

const std::string &MicrosoftAccounts::clientId()
{
	static const std::string id = OPTICRAFT_MSA_CLIENT_ID;
	return id;
}

bool MicrosoftAccounts::loginSupported()
{
	return PlatformAuth::microsoftLoginSupported() && !clientId().empty();
}

bool MicrosoftAccounts::hasAccount()
{
	if (!gLoaded)
		loadFromDisk();
	return !gAccount.username.empty() && !gAccount.uuid.empty();
}

const MicrosoftAccount &MicrosoftAccounts::account()
{
	if (!gLoaded)
		loadFromDisk();
	return gAccount;
}

void MicrosoftAccounts::applyToSession(Session *session)
{
	if (session != nullptr && hasAccount())
		session->username = gAccount.username;
}

void MicrosoftAccounts::save(const MicrosoftAccount &updated)
{
	gLoaded = true;
	gAccount = updated;
	const std::string path = accountPath();
	if (path.empty())
	{
		MC_LOG_WARN("auth", "accounts.nbt could not be written: no data dir\n");
		return;
	}
	try
	{
		std::unique_ptr<NBTTagCompound> root(new NBTTagCompound());
		root->setString("username", gAccount.username);
		root->setString("uuid", gAccount.uuid);
		root->setString("msaRefreshToken", gAccount.msaRefreshToken);
		root->setString("mcAccessToken", gAccount.mcAccessToken);
		root->setString("clientId", gAccount.clientId);
		root->setLong("mcTokenExpiresMs", gAccount.mcTokenExpiresMs);
		const std::vector<char> bytes = CompressedStreamTools::compress(root.get());
		const std::vector<unsigned char> outBytes(bytes.begin(), bytes.end());
		if (!PlatformStorage::writeFile(path, outBytes.data(), outBytes.size()))
			MC_LOG_WARN("auth", "accounts.nbt could not be written\n");
	}
	catch (const std::exception &exception)
	{
		MC_LOG_WARN("auth", "accounts.nbt could not be written: %s\n", exception.what());
	}
}

void MicrosoftAccounts::clear()
{
	gLoaded = true;
	gAccount = MicrosoftAccount();
	const std::string path = accountPath();
	if (!path.empty() && PlatformStorage::exists(path))
		PlatformStorage::removeFile(path);
}

bool MicrosoftAccounts::exchangeMsaAccessToken(const std::string &msaAccessToken,
                                               const std::string &usedClientId,
                                               MicrosoftAccount &outAccount,
                                               std::string &outError)
{
	outError.clear();

	// Xbox Live user token; uhs (user hash) is half of the identity token.
	const Headers jsonHeaders = {{"Content-Type", "application/json"},
	                              {"Accept", "application/json"},
	                              {"x-xbl-contract-version", "1"}};
	PlatformAuth::HttpResponse response;
	AuthJson::Value parsed;
	if (!performJson("POST", kXboxUserUrl, jsonHeaders,
	                 std::string("{\"Properties\":{\"AuthMethod\":\"RPS\","
	                             "\"SiteName\":\"user.auth.xboxlive.com\","
	                             "\"RpsTicket\":\"d=") +
	                 AuthJson::escape(msaAccessToken) +
	                 "\"},\"RelyingParty\":\"http://auth.xboxlive.com\",\"TokenType\":\"JWT\"}",
	                 response) ||
	    !AuthJson::parse(response.body, parsed) || parsed.member("Token") == nullptr)
	{
		outError = "Xbox Live could not be reached for sign-in";
		return false;
	}
	const std::string userToken = parsed.member("Token")->textOr("");
	std::string uhs;
	if (const AuthJson::Value *claims = parsed.member("DisplayClaims");
	    claims != nullptr && claims->member("xui") != nullptr && !claims->member("xui")->array.empty())
	{
		if (const AuthJson::Value *uhsMember = claims->member("xui")->array.front().member("uhs"))
			uhs = uhsMember->textOr("");
	}
	if (userToken.empty() || uhs.empty())
	{
		outError = "Xbox Live sign-in sent an unusable answer";
		return false;
	}

	// XSTS authorization for the Minecraft services.
	if (!performJson("POST", kXstsUrl, jsonHeaders,
	                 std::string("{\"Properties\":{\"SandboxId\":\"RETAIL\",\"UserTokens\":[\"") +
	                 AuthJson::escape(userToken) +
	                 "\"]},\"RelyingParty\":\"rp://api.minecraftservices.com/\",\"TokenType\":\"JWT\"}",
	                 response))
	{
		outError = "Microsoft's authorization service could not be reached";
		return false;
	}
	if (response.status == 401)
	{
		if (AuthJson::parse(response.body, parsed) && parsed.member("XErr") != nullptr)
		{
			long long xerr = 0;
			parsed.member("XErr")->numberAsInteger(xerr);
			outError = describeXstsError(xerr);
		}
		else
		{
			outError = "Microsoft refused to authorize this account for Minecraft";
		}
		return false;
	}
	if (!AuthJson::parse(response.body, parsed) || parsed.member("Token") == nullptr)
	{
		outError = "Microsoft's authorization service sent an unusable answer";
		return false;
	}
	const std::string xstsToken = parsed.member("Token")->textOr("");
	// The authorization must come back for the same user the user token
	// was issued to (PrismLauncher rejects a changed user hash the same
	// way); otherwise the identity token below is built from strangers.
	std::string xstsUhs;
	if (const AuthJson::Value *xstsClaims = parsed.member("DisplayClaims");
	    xstsClaims != nullptr && xstsClaims->member("xui") != nullptr &&
	    !xstsClaims->member("xui")->array.empty())
	{
		if (const AuthJson::Value *uhsMember = xstsClaims->member("xui")->array.front().member("uhs"))
			xstsUhs = uhsMember->textOr("");
	}
	if (xstsToken.empty() || xstsUhs.empty() || xstsUhs != uhs)
	{
		outError = "Microsoft's authorization service sent an unusable answer";
		return false;
	}

	// Minecraft services login.
	if (!performJson("POST", kMinecraftLoginUrl, jsonHeaders,
	                 std::string("{\"identityToken\":\"XBL3.0 x=") + AuthJson::escape(uhs) +
	                 ";" + AuthJson::escape(xstsToken) + "\"}",
	                 response) ||
	    !AuthJson::parse(response.body, parsed) || parsed.member("access_token") == nullptr)
	{
		outError = "Minecraft's login service could not be reached";
		return false;
	}
	const std::string mcAccessToken = parsed.member("access_token")->textOr("");
	long long expiresInSeconds = 86400;
	if (const AuthJson::Value *expires = parsed.member("expires_in"))
		expires->numberAsInteger(expiresInSeconds);

	// The account must own the game.
	if (!performJson("GET", kEntitlementsUrl,
	                 {{"Accept", "application/json"}, {"Authorization", "Bearer " + mcAccessToken}},
	                 "", response) ||
	    !AuthJson::parse(response.body, parsed) || parsed.member("items") == nullptr)
	{
		outError = "Minecraft's entitlement service could not be reached";
		return false;
	}
	// The store lists owned products by name (PrismLauncher reads the same
	// two flags): product_minecraft is the Java purchase, game_minecraft
	// covers Game Pass play. Anything else -- or nothing -- is not this game.
	bool ownsGame = false;
	for (const AuthJson::Value &item : parsed.member("items")->array)
	{
		const AuthJson::Value *name = item.member("name");
		if (name != nullptr &&
		    (name->textOr("") == "product_minecraft" || name->textOr("") == "game_minecraft"))
		{
			ownsGame = true;
			break;
		}
	}
	if (!ownsGame)
	{
		outError = "This Microsoft account does not own Minecraft: Java Edition";
		return false;
	}

	// The profile: name + id the client will identify with.
	if (!performJson("GET", kProfileUrl,
	                 {{"Accept", "application/json"}, {"Authorization", "Bearer " + mcAccessToken}},
	                 "", response) ||
	    !AuthJson::parse(response.body, parsed) ||
	    parsed.member("name") == nullptr || parsed.member("id") == nullptr)
	{
		outError = "The account's Minecraft profile could not be read";
		return false;
	}
	outAccount.username = parsed.member("name")->textOr("");
	// The profile id arrives dashed; the session join (and the stored
	// format) wants it undashed, so strip the dashes here once.
	outAccount.uuid = parsed.member("id")->textOr("");
	outAccount.uuid.erase(std::remove(outAccount.uuid.begin(), outAccount.uuid.end(), '-'),
	                      outAccount.uuid.end());
	outAccount.mcAccessToken = mcAccessToken;
	outAccount.clientId = usedClientId;
	outAccount.mcTokenExpiresMs = nowMs() + static_cast<long_t>(expiresInSeconds) * 1000L - kTokenSkewMs;
	if (outAccount.username.empty() || outAccount.uuid.empty())
	{
		outError = "The account's Minecraft profile is incomplete";
		return false;
	}
	return true;
}

bool MicrosoftAccounts::ensureFreshToken(std::string &outError)
{
	outError.clear();
	if (!hasAccount())
	{
		outError = "Not signed in";
		return false;
	}
	if (nowMs() < gAccount.mcTokenExpiresMs)
		return true;

	// Refresh grant for a new MSA token, then the shared chain again.
	// Microsoft rotates refresh tokens, so keep the replacement.
	PlatformAuth::HttpResponse response;
	PlatformAuth::HttpRequest request;
	request.url = kTokenUrl;
	request.method = "POST";
	request.headers = {{"Content-Type", "application/x-www-form-urlencoded"},
	                   {"Accept", "application/json"}};
	request.body = formBody({{"client_id", gAccount.clientId},
	                         {"grant_type", "refresh_token"},
	                         {"refresh_token", gAccount.msaRefreshToken},
	                         {"scope", "XboxLive.SignIn XboxLive.offline_access"}});
	AuthJson::Value parsed;
	if (!PlatformAuth::httpsRequest(request, response) || !AuthJson::parse(response.body, parsed) ||
	    parsed.member("access_token") == nullptr)
	{
		outError = "The saved login could not be refreshed; sign in again";
		return false;
	}
	const std::string msaAccessToken = parsed.member("access_token")->textOr("");
	std::string msaRefreshToken = gAccount.msaRefreshToken;
	if (const AuthJson::Value *refresh = parsed.member("refresh_token"))
		msaRefreshToken = refresh->textOr(msaRefreshToken);

	MicrosoftAccount refreshed = gAccount;
	if (!exchangeMsaAccessToken(msaAccessToken, gAccount.clientId, refreshed, outError))
		return false;
	refreshed.msaRefreshToken = msaRefreshToken;
	save(refreshed);
	return true;
}

bool MicrosoftAccounts::joinServer(const std::string &serverId, std::string &outError)
{
	if (!ensureFreshToken(outError))
		return false;
	const std::string body = std::string("{\"accessToken\":\"") + AuthJson::escape(gAccount.mcAccessToken) +
	                         "\",\"selectedProfile\":{\"id\":\"" + AuthJson::escape(gAccount.uuid) +
	                         "\",\"name\":\"" + AuthJson::escape(gAccount.username) +
	                         "\"},\"serverId\":\"" + AuthJson::escape(serverId) + "\"}";
	PlatformAuth::HttpResponse response;
	if (!performJson("POST", kSessionJoinUrl,
	                 {{"Content-Type", "application/json"}, {"Accept", "application/json"}},
	                 body, response))
	{
		outError = "Minecraft's session server could not be reached";
		return false;
	}
	if (response.status < 200 || response.status >= 300)
	{
		outError = "Minecraft's session server rejected this account (" +
		           std::to_string(response.status) + ")";
		return false;
	}
	return true;
}
