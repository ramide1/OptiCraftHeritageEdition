#include "net/minecraft/src/UiStrings.h"
#include "LegacyUiPolicy.h"

bool legacyUiDefaultEnabled()
{
    return true;
}

// hardcoded badd
const char *legacyUiTitleResourcePath()
{
    return "/legacy/title.png";
}

std::string legacyUiOptionLabel(bool enabled)
{
    return std::string(uiText("Legacy UI: ")) + (enabled ? uiText("ON") : uiText("OFF"));
}
