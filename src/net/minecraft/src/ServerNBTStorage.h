#pragma once

#include "java/String.h"
#include "java/Type.h"
#include "ProtocolVersion.h"
#include <mutex>

class NBTTagCompound;

// net.minecraft.src.ServerNBTStorage
class ServerNBTStorage
{
public:
	ServerNBTStorage(const jstring &name, const jstring &host,
	                 int_t version = ProtocolVersions::kAutoVersion);

	NBTTagCompound *getCompoundTag() const;
	static ServerNBTStorage *createServerNBTStorage(NBTTagCompound *tag);

	jstring name;
	jstring host;
	// Wire protocol to connect with (ProtocolVersions::kAutoVersion = follow
	// the global default, GameSettings::serverVersion). Persisted as the
	// "version" NBT tag in servers.dat; ids the build cannot speak stay raw
	// here and are clamped at join time (ProtocolVersions::resolveSupported)
	// so an options hand-edit never reaches Packet1Login.
	int_t version;
	jstring playerCount;
	jstring motd;
	long_t lag;
	bool polled;
#if defined(PS2_PLATFORM) || defined(CTR_PLATFORM)
	// The poll-retry ladder (GuiSlotServer / ThreadPollServers): both
	// consoles poll their rows through one narrow pipe -- the IOP RPC
	// bridge there, the single soc:U session here -- and gate dead rows so
	// they cannot re-claim it forever.
	long_t nextPollTime;
	int_t pollRetryCount;
#endif
	mutable std::mutex stateMutex;
};
