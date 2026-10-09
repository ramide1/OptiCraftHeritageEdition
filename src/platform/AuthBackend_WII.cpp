// AuthBackend_WII.cpp -- login transport stub. libogc ships no TLS stack
// and every endpoint of the Microsoft login flow is HTTPS-only, so Wii
// builds stay offline; the common flow code never sees a working transport
// and keeps the classic offline identity (GameSettings::playerName).
#include "platform/PlatformAuth.h"

bool PlatformAuth::microsoftLoginSupported()
{
	return false;
}

bool PlatformAuth::httpsRequest(const HttpRequest &request, HttpResponse &response)
{
	(void)request;
	(void)response;
	return false;
}
