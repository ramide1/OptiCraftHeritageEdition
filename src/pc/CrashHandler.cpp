#include "CrashHandler.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <cstdio>

namespace CrashHandler
{

void Crash(const std::string &message, const std::string &stackTrace)
{
	std::string text = message + "\n\n" + stackTrace;
#if defined(_WIN32)
	// Native dialog instead of SDL: the crash handler must not depend on a
	// subsystem that might be implicated in the crash. (The report also goes
	// to the log file.)
	MessageBoxW(nullptr, std::wstring(text.begin(), text.end()).c_str(),
	            L"Minecraft has crashed!", MB_ICONERROR | MB_OK);
#else
	// No dependency-free dialog on macOS/Linux — the report also goes to the
	// log; echo it to stderr so launching from a terminal still shows it.
	std::fputs(text.c_str(), stderr);
	std::fputc('\n', stderr);
#endif
}

}
