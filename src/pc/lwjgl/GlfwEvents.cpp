#include "pc/lwjgl/GlfwEvents.h"

#include <GLFW/glfw3.h>

#include "pc/lwjgl/Display.h"
#include "pc/lwjgl/GlfwKeymap.h"
#include "pc/lwjgl/Keyboard.h"
#include "pc/lwjgl/Mouse.h"
#if defined(MC_WIN32)
#include "pc/render/PcRenderBackend.h"
#include "pc/render/d3d9/PcD3D9Context.h"
#endif

namespace lwjgl
{
namespace detail
{

namespace
{

// GLFW reports absolute cursor positions only; relative motion is derived
// here from the last reported position.
double mouse_last_x = 0.0;
double mouse_last_y = 0.0;
bool mouse_have_origin = false;

// GLFW (unlike SDL) does not synthesize key-up events on focus loss, so the
// bridge tracks what is currently held and releases it itself.
bool keys_down[GLFW_KEY_LAST + 1] = {};
bool buttons_down[GLFW_MOUSE_BUTTON_LAST + 1] = {};

void releaseAllHeldInput(GLFWwindow *window)
{
	for (int key = 0; key <= GLFW_KEY_LAST; key++)
	{
		if (!keys_down[key])
			continue;
		keys_down[key] = false;
		const int lwjgl_key = Keyboard::detail::glfwKeyToLWJGL(key);
		if (lwjgl_key != Keyboard::KEY_NONE)
			Keyboard::detail::pushKey(lwjgl_key, false, false);
	}

	for (int button = 0; button <= GLFW_MOUSE_BUTTON_LAST; button++)
	{
		if (!buttons_down[button])
			continue;
		buttons_down[button] = false;
		double x = 0.0, y = 0.0;
		glfwGetCursorPos(window, &x, &y);
		Mouse::detail::pushButton(button, false, static_cast<int>(x), static_cast<int>(y));
	}

	resetGlfwMouseOrigin();
}

}

void resetGlfwMouseOrigin()
{
	mouse_have_origin = false;
}

void installGlfwEventCallbacks(GLFWwindow *window)
{
	glfwSetWindowCloseCallback(window, [](GLFWwindow *w)
	{
		// Latch the request and veto the close: the game owns the shutdown
		// path (isCloseRequested -> Minecraft shutdown -> window destroyed in
		// ~GLContext), exactly like the old SDL_QUIT handling.
		Display::detail::requestClose();
		glfwSetWindowShouldClose(w, GLFW_FALSE);
	});

	glfwSetKeyCallback(window, [](GLFWwindow *, int key, int, int action, int)
	{
		if (key < 0 || key > GLFW_KEY_LAST)
			return;
		const int lwjgl_key = Keyboard::detail::glfwKeyToLWJGL(key);
		if (lwjgl_key == Keyboard::KEY_NONE)
			return;

		const bool down = action != GLFW_RELEASE;
		const bool repeat = action == GLFW_REPEAT;
		keys_down[key] = down;
		Keyboard::detail::pushKey(lwjgl_key, down, repeat);
	});

	glfwSetCharCallback(window, [](GLFWwindow *, unsigned int codepoint)
	{
		// GLFW hands out codepoints directly, so the old UTF-8 -> UTF-32
		// conversion the SDL text-input path needed is gone.
		Keyboard::detail::pushChar(static_cast<int>(codepoint));
	});

	glfwSetCursorPosCallback(window, [](GLFWwindow *, double x, double y)
	{
		if (!mouse_have_origin)
		{
			// First event after creation / a warp: position without motion.
			mouse_last_x = x;
			mouse_last_y = y;
			mouse_have_origin = true;
			return;
		}

		const int xrel = static_cast<int>(x) - static_cast<int>(mouse_last_x);
		const int yrel = static_cast<int>(y) - static_cast<int>(mouse_last_y);
		mouse_last_x = x;
		mouse_last_y = y;
		Mouse::detail::pushMotion(static_cast<int>(x), static_cast<int>(y), xrel, yrel);
	});

	glfwSetMouseButtonCallback(window, [](GLFWwindow *w, int button, int action, int)
	{
		if (button < 0 || button > GLFW_MOUSE_BUTTON_LAST)
			return;

		double x = 0.0, y = 0.0;
		glfwGetCursorPos(w, &x, &y);
		const bool down = action != GLFW_RELEASE;
		buttons_down[button] = down;
		// GLFW numbers the first three buttons left/right/middle, which is
		// already the LWJGL order — no translation needed.
		Mouse::detail::pushButton(button, down, static_cast<int>(x), static_cast<int>(y));
	});

	glfwSetScrollCallback(window, [](GLFWwindow *w, double, double yoffset)
	{
		double x = 0.0, y = 0.0;
		glfwGetCursorPos(w, &x, &y);
		// Truncation mirrors the notch-granular wheel the game expects; the
		// few URLs and GUIs that read the wheel only ever compare against
		// +/-1. Sub-notch trackpad motion is dropped, like a notched wheel.
		Mouse::detail::pushWheel(static_cast<int>(yoffset), static_cast<int>(x), static_cast<int>(y));
	});

	glfwSetWindowFocusCallback(window, [](GLFWwindow *w, int focused)
	{
		if (focused != GLFW_FALSE)
			return;
		// Alt-tab while holding W must not leave the key stuck: SDL released
		// every held key/button on focus loss, GLFW leaves that to us.
		releaseAllHeldInput(w);
	});

	glfwSetFramebufferSizeCallback(window, [](GLFWwindow *, int, int)
	{
#if defined(MC_WIN32)
		if (pcRenderBackendIsDirect3D9())
			pcD3D9RequestResize();
#endif
	});
}

}
}
