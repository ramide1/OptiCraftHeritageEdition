#pragma once

#include "java/Type.h"

#include <string>

class Session;

// net.minecraft.src.MicrosoftAccount -- a signed-in Microsoft/Mojang
// account. One account per installation (the single player identity), stored
// in <minecraftDir>/accounts.nbt as gzipped NBT -- the same persistence
// family as servers.dat. All access goes through the MicrosoftAccounts
// namespace; the token cache refreshes lazily, so the only blocking work
// happens on the login worker thread (MicrosoftAuthFlow) or on the network
// reader thread during an online-mode join (NetClientHandler).
struct MicrosoftAccount
{
	std::string username;   // Minecraft profile name
	std::string uuid;       // profile id, undashed (the session join format)
	std::string msaRefreshToken;
	std::string mcAccessToken;
	std::string clientId;   // the client id that issued the tokens
	long_t mcTokenExpiresMs = 0;
};

namespace MicrosoftAccounts
{
	// The build-time client id (OPTICRAFT_MSA_CLIENT_ID). Empty means the
	// login flow cannot start even where the transport exists.
	const std::string &clientId();

	// True when this build could run the login flow at all (transport +
	// client id). Gates the login UI.
	bool loginSupported();

	bool hasAccount();
	const MicrosoftAccount &account();

	// Applies the stored profile name over the offline identity. Call after
	// options.txt has been read (it also overwrites session->username).
	void applyToSession(Session *session);

	void save(const MicrosoftAccount &updated);
	void clear();

	// Refresh + the Xbox/XSTS/Minecraft chain shared by first login and
	// re-auth: turns an MSA access token into a complete account (tokens,
	// expiry, profile). False + a player-readable outError on failure.
	bool exchangeMsaAccessToken(const std::string &msaAccessToken,
	                             const std::string &usedClientId,
	                             MicrosoftAccount &outAccount,
	                             std::string &outError);

	// Makes mcAccessToken usable; runs the refresh chain only when the
	// token is stale. False + a player-readable outError.
	bool ensureFreshToken(std::string &outError);

	// Registers the server hash with Mojang's session server -- the client
	// side of online mode. False + a player-readable outError.
	bool joinServer(const std::string &serverId, std::string &outError);
}
