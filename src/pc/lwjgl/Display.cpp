#include "lwjgl/Display.h"

#include <iostream>
#include <stdexcept>

#include "lwjgl/GLContext.h"

#include "external/GlfwException.h"
#include "pc/render/PcRenderBackend.h"
#if defined(MC_WIN32)
#include "pc/render/d3d9/PcD3D9Context.h"
#endif

#include <GLFW/glfw3.h>

namespace lwjgl
{
namespace Display
{

static bool close_requested = false;

static DisplayMode current_display_mode(0, 0);

// The windowed rect, stashed when going fullscreen so setFullscreen(false)
// can restore it — GLFW needs the position and size handed back explicitly,
// where SDL remembered them itself.
static int windowed_x = 0, windowed_y = 0;
static int windowed_width = 854, windowed_height = 480;

namespace detail
{

void requestClose()
{
	close_requested = true;
}

}

// Display functions
void setDisplayMode(const DisplayMode &display_mode)
{
	if (!display_mode.isFullscreen())
	{
		windowed_width = display_mode.getWidth();
		windowed_height = display_mode.getHeight();
		glfwSetWindowSize(GLContext::detail::getWindow(), windowed_width, windowed_height);
	}
	current_display_mode = display_mode;
	setFullscreen(display_mode.isFullscreen());
}

DisplayMode getDisplayMode()
{
	return current_display_mode;
}

void setTitle(const jstring &string)
{
	// I guess this gets ignored in favor of the frame title
	//
	(void)string;
}

void setFullscreen(bool fullscreen)
{
	GLFWwindow *window = GLContext::detail::getWindow();

	// No monitor switch needed when already in the requested state. This is
	// the windowed setDisplayMode() path at startup too: running it through
	// glfwSetWindowMonitor anyway recreates the Win32 window styles (which
	// drops the caption buttons until the next user resize) and stomps the
	// centered position with the still-zero windowed_x/windowed_y statics.
	if (fullscreen == (glfwGetWindowMonitor(window) != nullptr))
	{
		int w, h;
		glfwGetWindowSize(window, &w, &h);
		if (fullscreen)
			current_display_mode = DisplayMode(w, h, 32, glfwGetVideoMode(glfwGetPrimaryMonitor())->refreshRate);
		else
			current_display_mode = DisplayMode(w, h);
		return;
	}

	// Stash the windowed rect while still windowed, or the fullscreen size
	// would become the "restore" size.
	if (fullscreen && glfwGetWindowMonitor(window) == nullptr)
	{
		glfwGetWindowPos(window, &windowed_x, &windowed_y);
		glfwGetWindowSize(window, &windowed_width, &windowed_height);
	}

	GLFWmonitor *monitor = fullscreen ? glfwGetPrimaryMonitor() : nullptr;
	if (monitor != nullptr)
	{
		// Borderless desktop fullscreen, the GLFW equivalent of
		// SDL_WINDOW_FULLSCREEN_DESKTOP.
		const GLFWvidmode *mode = glfwGetVideoMode(monitor);
		glfwSetWindowMonitor(window, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
	}
	else
	{
		glfwSetWindowMonitor(window, nullptr, windowed_x, windowed_y,
		                      windowed_width, windowed_height, GLFW_DONT_CARE);
	}

	// Update display mode
	if (fullscreen)
	{
		int w, h;
		glfwGetWindowSize(window, &w, &h);
		current_display_mode = DisplayMode(w, h, 32, glfwGetVideoMode(glfwGetPrimaryMonitor())->refreshRate);
	}
	else
	{
		int w, h;
		glfwGetWindowSize(window, &w, &h);
		current_display_mode = DisplayMode(w, h);
	}
#if defined(MC_WIN32)
	if (pcRenderBackendIsDirect3D9())
	{
		pcD3D9RequestResize();
		pcD3D9ApplyPendingResize();
	}
#endif
}

bool isCloseRequested()
{
	return close_requested;
}

bool isVisible()
{
	return glfwGetWindowAttrib(GLContext::detail::getWindow(), GLFW_VISIBLE) != 0;
}

bool isActive()
{
	return glfwGetWindowAttrib(GLContext::detail::getWindow(), GLFW_FOCUSED) != 0;
}

void processMessages()
{
	// The event dispatch itself is callback-driven (see pc/lwjgl/GlfwEvents.cpp);
	// polling just pumps the GLFW event queue once per frame.
	glfwPollEvents();

	// Update display mode
	if (!current_display_mode.isFullscreen())
	{
		int w, h;
		glfwGetWindowSize(GLContext::detail::getWindow(), &w, &h);
		current_display_mode = DisplayMode(w, h);
	}
#if defined(MC_WIN32)
	if (pcRenderBackendIsDirect3D9())
		pcD3D9ApplyPendingResize();
#endif
}

void swapBuffers()
{
#if defined(MC_WIN32)
	if (pcRenderBackendIsDirect3D9())
	{
		pcD3D9Present();
		return;
	}
#endif
	glfwSwapBuffers(GLContext::detail::getWindow());
}

void update(bool doProcessMessages)
{
	swapBuffers();
	if (doProcessMessages)
		processMessages();
}

void create()
{
	GLFWwindow *window = GLContext::detail::getWindow();
	glfwShowWindow(window);
	// Latch the real on-screen rect now that the window is visible: the
	// windowed_x/y/width/height statics above still hold their defaults
	// (0,0,854,480), and a later fullscreen->windowed restore must hand
	// back where the window actually is, not the origin.
	glfwGetWindowPos(window, &windowed_x, &windowed_y);
	glfwGetWindowSize(window, &windowed_width, &windowed_height);
	// Center cursor on startup
	int w, h;
	glfwGetWindowSize(window, &w, &h);
	glfwSetCursorPos(window, w / 2.0, h / 2.0);
	// No SDL_StartTextInput equivalent: GLFW delivers character events
	// unconditionally, and the game only ever wanted them flowing.
}

int_t getX()
{
	int x;
	glfwGetWindowPos(GLContext::detail::getWindow(), &x, nullptr);
	return x;
}

int_t getY()
{
	int y;
	glfwGetWindowPos(GLContext::detail::getWindow(), nullptr, &y);
	return y;
}

int_t getWidth()
{
	return current_display_mode.getWidth();
}

int_t getHeight()
{
	return current_display_mode.getHeight();
}

}
}
