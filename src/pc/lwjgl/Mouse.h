#pragma once

#include "java/Type.h"

namespace lwjgl
{
namespace Mouse
{
namespace detail
{

// Every platform feeds mouse events through the same pushers:
//   PS2 — right stick drives a simulated cursor.
//   Wii — the Wiimote IR pointer drives it directly, with the Classic/GC right
//         stick as the fallback when the pointer is off-screen.
//   3DS — the touch screen drives it directly (bottom LCD, already absolute
//         in top-screen pixels; see src/3ds/input/DsInput).
//   PC  — the GLFW callback bridge (pc/lwjgl/GlfwEvents.cpp).
// Coordinates arriving here are top-left origin; each implementation stores
// or flips them as it needs, and the public getX()/getY()/getEventY() accessors
// are bottom-left origin per LWJGL.
void pushMotion(int x, int y, int xrel, int yrel);
void pushButton(int button, bool down, int x, int y);
void pushWheel(int delta, int x, int y);

}

void setCursorPosition(int_t x, int_t y);

// Event handling
bool next();

int_t getEventButton();
bool getEventButtonState();

int_t getEventDX();
int_t getEventDY();

int_t getEventX();
int_t getEventY();

int_t getEventDWheel();

// State
int_t getX();
int_t getY();

int_t getDX();
int_t getDY();

int_t getDWheel();

// Clears accumulated relative mouse motion and wheel deltas.
// Useful after grabbing/ungrabbing the mouse so stale menu or warp deltas
// do not rotate the camera on the first gameplay frames.
void clearDeltas();

bool isButtonDown(int_t button);

bool isGrabbed();
void setGrabbed(bool grabbed);

}
}
