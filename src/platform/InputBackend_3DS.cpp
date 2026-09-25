// InputBackend_3DS.cpp -- the 3DS half of the shared platform Input API.
//
// Deliberately thin, like InputBackend_WII.cpp: src/3ds/input/DsInput.cpp
// owns the libctru scan and produces one snapshot per frame (buttons ->
// PLATFORM_TEXT_* mask for an open screen, the gameplay channel for a closed
// one, touch -> absolute pointer in top-screen pixels, circle pad -> stick
// axes), and this file only reshapes it into the shared structs. The
// circle-pad axes ride in platformGamepadSnapshot() exactly as the Wii's
// stick snapshot does; there is no second stick, so the right-side fields
// stay zero and raw == filtered (DsInput.h keeps deadzones downstream).
//
// Note on the mode flags Input.h also declares (text-input-exclusive,
// container-navigation, pad-rebind): those are NOT part of the backend half --
// src/platform/Input.cpp is a common source compiled into every target and
// already owns all three flag pairs, on every platform. Re-defining them here
// would be a duplicate-symbol link error; PS2/Wii/PC backends don't define
// them either.

#include "platform/Input.h"

#include "3ds/input/DsInput.h"

PlatformTextInputSnapshot platformTextInputSnapshot(int port)
{
    (void)port; // one input source; there is no second pad to address.
    PlatformTextInputSnapshot out;
    const DsInputState& ds = dsInputState();
    out.connected = ds.connected;
    out.held = ds.held;
    out.pressed = dsInputConsumePressed(); // consume-on-read, same as Wii/PS2
    out.pointerValid = ds.pointerActive;
    out.pointerX = ds.pointerX;
    out.pointerY = ds.pointerY;
    // DsInput already maps touch into top-screen pixels, and this panel is
    // fixed 400x240 hardware -- so the pointerX * screenW / pointerWidth
    // rescale VirtualKeyboard applies degenerates to an identity, which is
    // exactly what we want.
    out.pointerWidth = 400;
    out.pointerHeight = 240;
    return out;
}

PlatformGamepadSnapshot platformGamepadSnapshot(int port)
{
    (void)port;
    PlatformGamepadSnapshot out;
    const DsInputState& ds = dsInputState();
    out.connected = ds.stickConnected;
    out.leftX = ds.stickX;
    out.leftY = ds.stickY; // up-positive, same convention as the Wii stick
    return out;
}

PlatformGamepadSnapshot platformRawGamepadSnapshot(int port)
{
    // Already raw: DsInput publishes unfiltered -1..1 axes (deadzone handling
    // is downstream), so there is no filtered variant to distinguish from.
    return platformGamepadSnapshot(port);
}

int platformMenuPad()
{
    return 0; // single pad, no PS2-style owner hand-off
}

bool platformMenuPointerActive()
{
    // Finger down = the touch pointer owns the GUI this frame; finger up
    // hands control back to D-pad/stick navigation (same shape as the Wii's
    // dynamic pointer ownership).
    return dsInputState().pointerActive;
}

bool platformMenuCursorVisible()
{
    return true; // software cursor, touch-driven (PLATFORM_SOFTWARE_CURSOR)
}

void platformSetMenuCursor(int x, int y)
{
    // phase 1: container-navigation cursor stepping; the absolute touch
    // pointer overwrites the cursor position on its next contact anyway.
    (void)x;
    (void)y;
}

const PlatformKeyboardHints& platformKeyboardHints()
{
    // The on-screen keyboard's legend, i.e. the MENU channel only: these are
    // the PLATFORM_TEXT_* meanings mapTextButtons() assigns. Gameplay bindings
    // live in GameSettings (platformGameSettingsInitialize) and are shown by
    // the Controls screen instead.
    static const PlatformKeyboardHints hints = {
        {
            "A:type B:back D-pad:move",
            "X/L:sp Y:close R:shift",
            "Sel:sp St:esc"
        }, 3
    };
    return hints;
}

const char* platformInputDebugLine()
{
    return dsInputDebugLine();
}
