#include "System.h"

#include <chrono>
#include <string>
#if !defined(PS2_PLATFORM) && !defined(WII_PLATFORM) && !defined(CTR_PLATFORM)
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#else
#include <cstdlib>
#endif
#endif

namespace System
{

long_t currentTimeMillis()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

long_t nanoTime()
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool openURL(const std::string &url)
{
#if defined(PS2_PLATFORM) || defined(WII_PLATFORM) || defined(CTR_PLATFORM)
	// No browser to hand the URL to on any of the three: the 3DS has the HOME
	// browser, but libctru exposes no supported way to hand it a URL.
	(void)url;
	return false;
#elif defined(_WIN32)
	// ShellExecuteW returns a value > 32 on success (the old SDL_OpenURL
	// ended up here anyway). URLs are ASCII, so the naive widening is fine.
	const std::wstring wide(url.begin(), url.end());
	return reinterpret_cast<intptr_t>(ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
#elif defined(__APPLE__)
	return std::system(("open '" + url + "'").c_str()) == 0;
#else
	// Single-quoted so '&' and '?' in the URL cannot reach a shell metachar;
	// the game only ever opens plain http(s) links.
	return std::system(("xdg-open '" + url + "'").c_str()) == 0;
#endif
}

}
