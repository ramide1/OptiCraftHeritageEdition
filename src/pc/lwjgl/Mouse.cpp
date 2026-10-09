#include "lwjgl/Mouse.h"

#include <queue>

#include "lwjgl/GLContext.h"
#include "lwjgl/Display.h"
#include "pc/lwjgl/GlfwEvents.h"

#include <GLFW/glfw3.h>

namespace lwjgl
{
namespace Mouse
{

static int_t staging_dx = 0;
static int_t staging_dy = 0;
static int_t staging_dz = 0;

static bool grabbed = false;

namespace detail
{

struct Event
{
	int8_t button, down;
	int32_t x, y;
	int32_t xrel, yrel;
	int32_t wheel;

	Event(int8_t button = 0, int8_t down = 0, int32_t x = 0, int32_t y = 0, int32_t xrel = 0, int32_t yrel = 0, int32_t wheel = 0)
		: button(button), down(down), x(x), y(y), xrel(xrel), yrel(yrel), wheel(wheel)
	{ }
};

static Event event_current = {};
static std::queue<Event> event_queue;

// The contract the console implementations share (see pc/lwjgl/Mouse.h):
// incoming coordinates are top-left origin with top-left-relative deltas, and
// the stored event is bottom-left origin with bottom-left-relative deltas, so
// the public accessors hand them back LWJGL-style without flipping there.
void pushMotion(int x, int y, int xrel, int yrel)
{
	staging_dx += xrel;
	staging_dy -= yrel;
	event_queue.emplace(-1, 0, x, Display::getHeight() - y - 1, xrel, -yrel, 0);
}

void pushButton(int button, bool down, int x, int y)
{
	// GLFW already numbers the first buttons left/right/middle, which is the
	// LWJGL order (SDL needed the left/right/middle -> 0/1/2 swap).
	event_queue.emplace(static_cast<int8_t>(button), down ? 1 : 0, x, Display::getHeight() - y - 1, 0, 0, 0);
}

void pushWheel(int delta, int x, int y)
{
	// Accumulate like the console implementations do — the SDL-era desktop
	// never touched staging_dz here, so Mouse::getDWheel() was dead zero.
	staging_dz += delta;
	event_queue.emplace(-1, 0, x, Display::getHeight() - y - 1, 0, 0, delta);
}

}

void setCursorPosition(int_t x, int_t y)
{
	// LWJGL coordinates are bottom-left; GLFW's cursor API is top-left.
	glfwSetCursorPos(GLContext::detail::getWindow(), x, Display::getHeight() - y - 1);
}

// Event handling
bool next()
{
	if (detail::event_queue.empty())
		return false;
	detail::event_current = detail::event_queue.front();
	detail::event_queue.pop();
	return true;
}

int_t getEventButton()
{
	return detail::event_current.button;
}
bool getEventButtonState()
{
	return detail::event_current.down != 0;
}

int_t getEventDX()
{
	return detail::event_current.xrel;
}

int_t getEventDY()
{
	return detail::event_current.yrel;
}

int_t getEventX()
{
	return detail::event_current.x;
}

int_t getEventY()
{
	return detail::event_current.y;
}

int_t getEventDWheel()
{
	return detail::event_current.wheel;
}

// State
int_t getX()
{
	double x = 0.0;
	glfwGetCursorPos(GLContext::detail::getWindow(), &x, nullptr);
	return (int_t)x;
}

int_t getY()
{
	double y = 0.0;
	glfwGetCursorPos(GLContext::detail::getWindow(), nullptr, &y);
	return lwjgl::Display::getHeight() - (int_t)y - 1;
}

int_t getDX()
{
	int_t result = staging_dx;
	staging_dx = 0;
	return result;
}

int_t getDY()
{
	int_t result = staging_dy;
	staging_dy = 0;
	return result;
}

int_t getDWheel()
{
	int_t result = staging_dz;
	staging_dz = 0;
	return result;
}

void clearDeltas()
{
	staging_dx = 0;
	staging_dy = 0;
	staging_dz = 0;

	// The GLFW bridge derives relative motion from the last reported cursor
	// position; dropping that origin prevents the next motion event after a
	// warp from carrying a bogus camera delta.
	lwjgl::detail::resetGlfwMouseOrigin();
}

bool isButtonDown(int_t button)
{
	if (button < 0 || button > GLFW_MOUSE_BUTTON_LAST)
		return false;
	return glfwGetMouseButton(GLContext::detail::getWindow(), static_cast<int>(button)) == GLFW_PRESS;
}

bool isGrabbed()
{
	return grabbed;
}

void setGrabbed(bool state)
{
	grabbed = state;

	staging_dx = 0;
	staging_dy = 0;

	GLFWwindow *window = GLContext::detail::getWindow();
	glfwSetInputMode(window, GLFW_CURSOR, grabbed ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
	if (glfwRawMouseMotionSupported())
		glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, grabbed ? GLFW_TRUE : GLFW_FALSE);
	clearDeltas();
}

}
}
