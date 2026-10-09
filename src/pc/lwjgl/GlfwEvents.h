#pragma once

// Desktop-only GLFW event bridge. GLFW is callback-driven rather than
// pump-driven like SDL was, so the window (created in GLContext.cpp) installs
// these callbacks at creation time; they translate GLFW events into the same
// neutral detail::push* queues the console input polls feed.

struct GLFWwindow;

namespace lwjgl
{
namespace detail
{

// Installs all event callbacks on a freshly created window. Must be called
// before the window is shown / any events can arrive.
void installGlfwEventCallbacks(GLFWwindow *window);

// Forgets the last reported cursor position so the next motion event does not
// derive a bogus relative delta from it (after a grab/ungrab or cursor warp).
void resetGlfwMouseOrigin();

}
}
