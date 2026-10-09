#include "platform/Input.h"

#include "platform/Log.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_joystick.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>

// PC gamepad through SDL3 -- and ONLY through SDL3's gamepad/joystick
// subsystems (etapa 3 of the 2026-10 migration). Window, GL context and
// keyboard/mouse stay on GLFW; audio stays on OpenAL; networking moved to
// SDL3_net. SDL_INIT_GAMEPAD is the only subsystem ever initialised here,
// so no SDL video/audio/event-loop piece can leak back in.
//
// Design mirrors the console backends' cooked/raw split:
//
//   * platformGamepadSnapshot()    -- deadzone+rescale cooked axes, the same
//     0.20 gate and linear rescale InputBackend_3DS/InputBackend_PS2 apply
//     (Ps2AnalogFilter), so MovementInputFromOptions, the direct pad camera
//     and GuiScreen's own +-0.20 menu threshold behave identically.
//   * platformRawGamepadSnapshot() -- unfiltered, for GuiDeadzoneSettings
//     calibration and any future tuning UI.
//
// Axis convention matches the consoles: down-positive (stick up reads
// negative), which is also SDL's own gamepad convention and the
// raw-joystick contract MovementInputFromOptions negates into "forward".
//
// Polling, not events: every snapshot call pumps (SDL_PumpEvents feeds the
// hotplug watcher, SDL_UpdateJoysticks refreshes state) under one mutex,
// opens the first available gamepad, and re-opens on disconnect. Unknown
// controllers get SDL's fallback mapping, so any twin-stick USB pad works;
// exotic devices are best-effort. Rescans while padless are throttled to
// ~500 ms so an idle menu doesn't allocate a joystick list every frame.
// Buttons are deliberately NOT mapped here (sticks + menus scope): the
// snapshot struct carries axes only, and keyboard/mouse stay authoritative.

namespace
{
constexpr float kPcStickDeadzone = 0.20f;
constexpr std::chrono::milliseconds kPadlessRescanInterval(500);

float applyStickDeadzone(float value)
{
    if (value > -kPcStickDeadzone && value < kPcStickDeadzone)
        return 0.0f;
    const float sign = value < 0.0f ? -1.0f : 1.0f;
    float magnitude = (std::abs(value) - kPcStickDeadzone) / (1.0f - kPcStickDeadzone);
    if (magnitude < 0.0f)
        magnitude = 0.0f;
    if (magnitude > 1.0f)
        magnitude = 1.0f;
    return magnitude * sign;
}

float normalizeAxis(Sint16 value)
{
    float out = value >= 0 ? static_cast<float>(value) / 32767.0f
                           : static_cast<float>(value) / 32768.0f;
    if (out < -1.0f)
        out = -1.0f;
    if (out > 1.0f)
        out = 1.0f;
    return out;
}

std::mutex s_padMutex;
SDL_Gamepad *s_pad = nullptr;
bool s_gamepadInitTried = false;
bool s_gamepadInitOk = false;
std::chrono::steady_clock::time_point s_lastPadlessScan{};

void ensureGamepadInitLocked()
{
    if (s_gamepadInitTried)
        return;
    s_gamepadInitTried = true;
    // GAMEPAD implies JOYSTICK (and EVENTS for hotplug); nothing else.
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
    {
        MC_LOG_WARN("input", "PC gamepad disabled: SDL gamepad init failed: %s\n",
            SDL_GetError());
        return;
    }
    s_gamepadInitOk = true;
}

void pollGamepadLocked()
{
    SDL_PumpEvents();
    SDL_UpdateJoysticks();

    if (s_pad != nullptr && !SDL_GamepadConnected(s_pad))
    {
        SDL_CloseGamepad(s_pad);
        s_pad = nullptr;
    }
    if (s_pad != nullptr)
        return;

    const auto now = std::chrono::steady_clock::now();
    if (now - s_lastPadlessScan < kPadlessRescanInterval)
        return;
    s_lastPadlessScan = now;

    int count = 0;
    SDL_JoystickID *ids = SDL_GetJoysticks(&count);
    if (ids == nullptr)
        return;
    for (int i = 0; i < count && s_pad == nullptr; ++i)
        s_pad = SDL_OpenGamepad(ids[i]);
    SDL_free(ids);
}

PlatformGamepadSnapshot readPadSnapshot(bool cooked)
{
    PlatformGamepadSnapshot out;
    std::lock_guard<std::mutex> guard(s_padMutex);
    ensureGamepadInitLocked();
    if (!s_gamepadInitOk)
        return out;
    pollGamepadLocked();
    if (s_pad == nullptr)
        return out;

    out.connected = true;
    const float lx = normalizeAxis(SDL_GetGamepadAxis(s_pad, SDL_GAMEPAD_AXIS_LEFTX));
    const float ly = normalizeAxis(SDL_GetGamepadAxis(s_pad, SDL_GAMEPAD_AXIS_LEFTY));
    const float rx = normalizeAxis(SDL_GetGamepadAxis(s_pad, SDL_GAMEPAD_AXIS_RIGHTX));
    const float ry = normalizeAxis(SDL_GetGamepadAxis(s_pad, SDL_GAMEPAD_AXIS_RIGHTY));
    if (cooked)
    {
        out.leftX = applyStickDeadzone(lx);
        out.leftY = applyStickDeadzone(ly);
        out.rightX = applyStickDeadzone(rx);
        out.rightY = applyStickDeadzone(ry);
    }
    else
    {
        out.leftX = lx;
        out.leftY = ly;
        out.rightX = rx;
        out.rightY = ry;
    }
    return out;
}
} // namespace

PlatformTextInputSnapshot platformTextInputSnapshot(int port)
{
    (void)port;
    return {};
}

PlatformGamepadSnapshot platformGamepadSnapshot(int port)
{
    if (port != 0)
        return {};
    return readPadSnapshot(true);
}

PlatformGamepadSnapshot platformRawGamepadSnapshot(int port)
{
    if (port != 0)
        return {};
    return readPadSnapshot(false);
}

int platformMenuPad()
{
    return 0;
}

bool platformMenuPointerActive()
{
    return true;
}

bool platformMenuCursorVisible()
{
    return false;
}

void platformSetMenuCursor(int x, int y)
{
    (void)x;
    (void)y;
}

const PlatformKeyboardHints& platformKeyboardHints()
{
    static const PlatformKeyboardHints hints;
    return hints;
}

const char* platformInputDebugLine()
{
    return "";
}
