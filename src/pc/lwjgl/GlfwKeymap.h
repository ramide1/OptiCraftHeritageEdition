#pragma once

// Desktop-only key-code translation between GLFW's key codes and the LWJGL
// DirectInput-style codes the Key enum in pc/lwjgl/Keyboard.h uses. The event
// bridge (GlfwEvents.cpp) needs GLFW->LWJGL; Keyboard.cpp needs LWJGL->GLFW
// for glfwGetKey polling. Both directions come from the same pair table so
// they can never drift apart.

#include "pc/lwjgl/Keyboard.h"

namespace lwjgl
{
namespace Keyboard
{
namespace detail
{

// Returns the LWJGL code for a GLFW key, or KEY_NONE when unmapped.
int glfwKeyToLWJGL(int glfwKey);

// Returns the GLFW key for an LWJGL code, or GLFW_KEY_UNKNOWN when unmapped.
int_t lwjglKeyToGLFW(int_t lwjglKey);

}
}
}
