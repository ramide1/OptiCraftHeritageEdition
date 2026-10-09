#pragma once

#include <atomic>
#include <string>

#include "java/Type.h"
#include "platform/Mutex.h"
#include "platform/Thread.h"

// net.minecraft.src.MicrosoftAuthFlow -- the device-code login worker
// (RFC 8628): asks Microsoft for a device code, publishes it for the screen
// to show, then polls the token endpoint until the user finishes the sign-in
// on another device (or the code expires). Runs its own thread exactly like
// ThreadConnectToServer; the login screen polls takeSnapshot() once per
// frame. On success the account is already saved (MicrosoftAccounts::save)
// -- the GUI only refreshes its state.
class MicrosoftAuthFlow
{
public:
	enum class State
	{
		RequestingDeviceCode,
		WaitingForUser,
		ExchangingTokens, // Xbox -> XSTS -> Minecraft services
		Done,
		Failed
	};

	struct Snapshot
	{
		State state = State::RequestingDeviceCode;
		std::string verificationUrl;
		std::string userCode;
		std::string error;
		bool finished = false;
	};

	MicrosoftAuthFlow() = default;
	~MicrosoftAuthFlow();

	void start();
	void cancel();
	// True when a fresh snapshot arrived; the GUI re-renders then.
	bool takeSnapshot(Snapshot &out);

private:
	static void *threadEntry(void *argument);
	void run();
	void publish(const Snapshot &next);
	// Sleeps in 250 ms slices so cancel() answers within a moment.
	bool waitCancellable(int_t seconds);

	PlatformMutex lock;
	Snapshot snapshot;
	bool snapshotFresh = false;
	std::atomic_bool cancelled{false};
	PlatformThread worker;
};
