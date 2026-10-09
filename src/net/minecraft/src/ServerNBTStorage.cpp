#include "ServerNBTStorage.h"

#include "NBTTagCompound.h"

ServerNBTStorage::ServerNBTStorage(const jstring &serverName, const jstring &serverHost, int_t serverVersion) :
	name(serverName), host(serverHost), version(serverVersion), playerCount(), motd(), lag(0), polled(false)
#if defined(PS2_PLATFORM) || defined(CTR_PLATFORM)
	, nextPollTime(0), pollRetryCount(0)
#endif
{
}

NBTTagCompound *ServerNBTStorage::getCompoundTag() const
{
	NBTTagCompound *tag = new NBTTagCompound();
	tag->setString("name", name);
	tag->setString("ip", host);
	tag->setInteger("version", version);
	return tag;
}

ServerNBTStorage *ServerNBTStorage::createServerNBTStorage(NBTTagCompound *tag)
{
	if (tag == nullptr)
		return nullptr;
	const int_t serverVersion = tag->hasKey("version")
		? tag->getInteger("version") : ProtocolVersions::kAutoVersion;
	return new ServerNBTStorage(tag->getString("name"), tag->getString("ip"), serverVersion);
}
