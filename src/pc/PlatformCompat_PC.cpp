#include "platform/PlatformCompat.h"

// Desktop implementation of the one PlatformCompat entry point that needs a
// platform header: keeping <windows.h> (and its min/max macro pollution) out
// of the widely-included PlatformCompat.h is worth the extra translation
// unit, the same way Resources_PC.cpp keeps the filesystem details out of it.

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace PlatformCompat
{

void setSmoothInputThreadPriority(bool enabled)
{
	// Java C6 runs the game thread at priority 10 and drops it to 5 with
	// Smooth Input; the Win32 mapping is HIGHEST vs NORMAL. Cache the applied
	// state so the OS priority API is touched only when the option changes,
	// not every frame.
	static int applied_state = -1;
	const int requested_state = enabled ? 1 : 0;
	if (applied_state == requested_state)
		return;

	if (SetThreadPriority(GetCurrentThread(), enabled ? THREAD_PRIORITY_NORMAL : THREAD_PRIORITY_HIGHEST))
		applied_state = requested_state;
}

}

#else

namespace PlatformCompat
{

void setSmoothInputThreadPriority(bool)
{
	// POSIX has no unprivileged way to raise the main thread's priority
	// (SCHED_OTHER tops out at nice 0, and SCHED_FIFO needs root/caps), so
	// the option stays Windows-only — the same platform-policy call the
	// console ports make when they no-op it.
}

}

#endif
