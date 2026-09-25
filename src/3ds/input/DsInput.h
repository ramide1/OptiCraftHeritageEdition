#pragma once

#include <cstdint>

#include "platform/Input.h" // PLATFORM_TEXT_* action masks

// Touchscreen + circle-pad input for the 3DS port (src/3ds/input/DsInput.cpp).
//
// One snapshot per frame -- what platform/InputBackend_3DS.cpp feeds into the
// shared Input API -- plus the key/mouse/wheel edges the engine plays with.
// Every physical button has up to two channels, because an open screen and a
// closed one give it different meanings; DsInput.cpp's header carries the
// full button -> channel table.
//
// Coordinates: the game surface is the top screen (400x240), the touch panel
// the bottom screen (320x240). Touch positions are mapped to top-screen
// pixels inside DsInput (x * 400/320, y unchanged), so callers can compare
// them against the 400x240 GUI directly. Revisit when the on-screen touch
// controls land.

struct DsInputState
{
    bool connected = true;   // the console itself is always "attached"
    std::uint32_t held = 0;  // PLATFORM_TEXT_* actions held right now

    bool pointerActive = false; // finger down during this poll
    int pointerX = 0;           // top-screen pixels; valid while pointerActive
    int pointerY = 0;

    bool stickConnected = true; // circle pad present (always true on 3DS)
    float stickX = 0.0f;        // -1..1, raw (deadzone handling is downstream)
    float stickY = 0.0f;
};

// One-time setup: remember the top-screen size used for the touch mapping.
// Called from lwjgl::Display::create().
void dsInputInit(int screenW, int screenH);

// hidScanInput() + refresh the snapshot; called once per frame from
// lwjgl::Display::processMessages(). Also forwards touch -> mouse, START ->
// KEY_ESCAPE, and the gameplay channel (jump/inventory/sneak/movement keys,
// attack/use mouse buttons, hotbar wheel).
//
// inMenu is "a GuiScreen is currently open", which the input layer cannot
// work out for itself -- the same reason WiiPadState::wiiPadPoll() and
// Ps2Input::update() take it as a parameter (see the header comment in
// DsInput.cpp for what it gates).
void dsInputPoll(bool inMenu);

const DsInputState& dsInputState();

// PLATFORM_TEXT_* actions that went down since the last call: returns the
// accumulated edge mask and clears it (consume-on-read, same contract as the
// Wii's wiiTextInputConsumePressed()).
std::uint32_t dsInputConsumePressed();

// One-line human-readable state for the debug overlay, or nullptr when idle.
const char* dsInputDebugLine();
