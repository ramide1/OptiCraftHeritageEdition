#include "platform/ClientPlatformPolicy.h"

#include "net/minecraft/src/GameResources.h"
#include "pc/CrashHandler.h"

namespace ClientPlatformPolicy
{
int initialWidth()
{
    // Phase-1 constant: the 3DS top screen is 400x240. lwjgl::Display does
    // declare getWidth()/getHeight(), but nothing owns them until the
    // citro3d renderer creates the framebuffer, so reading them pre-init
    // would report whatever a stub backend left behind -- the constant also
    // compiles without pulling Display into this target's link. Once the
    // renderer lands, lwjgl::Display owns this value the way the Wii's
    // wiigl_width() owns it there.
    return 400;
}

int initialHeight()
{
    // Top screen of the same 400x240 pair; see initialWidth() for why this
    // is a constant rather than a Display query.
    return 240;
}

std::string minecraftDirectory()
{
    return GameResources::getExeDir() + "/.minecraft";
}

bool saveConverterUsesSavesSubdirectory()
{
    return true;
}

void applyGameSettingsDefaults(GameSettings*)
{
}

void preloadStartupTextures(RenderEngine*)
{
}

void releaseWorldEntryAssets(RenderEngine*)
{
}

int panoramaSampleGrid()
{
    return 8;
}

void reportCrash(const std::string& description)
{
    CrashHandler::Crash(description);
}
}
