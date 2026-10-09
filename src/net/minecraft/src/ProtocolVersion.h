#pragma once

#include "java/Type.h"

#include <array>
#include <cstddef>
#include <string>

// Per-server / global server-version selection (the ViaFabricPlus-style
// "Set version" scheme). The client engine natively speaks a single wire
// protocol; this registry is the one source of truth for which *target*
// versions a build can actually speak, so the multiplayer UI never offers a
// version the translation layer cannot deliver. Entries marked unsupported
// exist so the server list and the (future) ping autodetection can name
// them; they become selectable the day a translation adapter for them lands.
//
// Wire numbers are the pre-Netty protocol ids of the Minecraft handshake
// (minecraft.wiki, "Protocol version numbers"); Packet1Login carries one to
// the server during login.

struct ProtocolVersionInfo
{
	int_t protocolId;
	const char *displayName;
	bool supported;
};

namespace ProtocolVersions
{
	// Sentinel stored in servers.dat ("version" tag) and shown as "Auto":
	// follow the global default (GameSettings::serverVersion) instead of
	// pinning a specific version for this server.
	constexpr int_t kAutoVersion = 0;

	// The wire protocol this client implements natively (Minecraft 1.2.5).
	constexpr int_t kNativeVersion = 29;

	// Ordered oldest-first; the Add/Edit Server selector cycles this list.
	inline const std::array<ProtocolVersionInfo, 6> &all()
	{
		static constexpr std::array<ProtocolVersionInfo, 6> kVersions = {{
			{14, "Beta 1.7.3", false},  // first planned translation target
			{17, "Beta 1.8.1", false},
			{22, "1.0.0", false},
			{23, "1.1", false},
			{28, "1.2.3", false},
			{29, "1.2.5", true},        // native engine protocol
		}};
		return kVersions;
	}

	inline int_t count()
	{
		return static_cast<int_t>(all().size());
	}

	// Index of protocolId in the registry, -1 when the id is unknown.
	inline int_t indexOf(int_t protocolId)
	{
		const auto &versions = all();
		for (std::size_t i = 0; i < versions.size(); i++)
		{
			if (versions[i].protocolId == protocolId)
				return static_cast<int_t>(i);
		}
		return -1;
	}

	// protocolId when this build supports it, the native version otherwise.
	// Hand-edited (or newer-build) servers.dat/options.txt values must never
	// reach Packet1Login before the matching adapter exists.
	inline int_t resolveSupported(int_t protocolId)
	{
		const int_t index = indexOf(protocolId);
		return index >= 0 && all()[static_cast<std::size_t>(index)].supported
			? protocolId : kNativeVersion;
	}

	// Human-readable name for a wire protocol id ("Unknown (<id>)" fallback).
	inline std::string displayName(int_t protocolId)
	{
		const int_t index = indexOf(protocolId);
		if (index >= 0)
			return all()[static_cast<std::size_t>(index)].displayName;
		return "Unknown (" + std::to_string(static_cast<int>(protocolId)) + ")";
	}
}
