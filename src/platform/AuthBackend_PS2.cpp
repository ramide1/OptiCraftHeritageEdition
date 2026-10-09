// AuthBackend_PS2.cpp -- login transport stub. PS2SDK has no TLS stack
// in-tree and every endpoint of the Microsoft login flow is HTTPS-only, so
// PS2 builds stay offline; the common flow code never sees a working
// transport and keeps the classic offline identity (GameSettings::playerName).
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
