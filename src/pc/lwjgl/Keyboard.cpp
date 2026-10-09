#include "lwjgl/Keyboard.h"

#include <queue>

#include "lwjgl/GLContext.h"
#include "lwjgl/KeyNames.h"
#include "pc/lwjgl/GlfwKeymap.h"

#include <GLFW/glfw3.h>

namespace lwjgl
{
namespace Keyboard
{

namespace detail
{

struct Event
{
	int_t key;
	int_t character;
	bool repeat;
	bool down;
};

static Event event_current = {};
static std::queue<Event> event_queue;

static bool has_retained_event = false;
static Event event_retained = {};

static void flushRetained()
{
	if (!has_retained_event)
		return;

	has_retained_event = false;
	event_queue.push(event_retained);
}

static void handleKey(int_t key, bool repeat, bool down)
{
	flushRetained();
	has_retained_event = true;

	event_retained.key = key;
	event_retained.character = 0;
	event_retained.repeat = repeat;
	event_retained.down = down;
}

static void handleCharacter(int_t character)
{
	if (has_retained_event && event_retained.character != 0)
		flushRetained();
	if (!has_retained_event)
		event_queue.push({KEY_NONE, character, false, true});
	else
		event_retained.character = character;
}

void pushKey(int lwjglKey, bool down, bool repeat)
{
	handleKey(lwjglKey, repeat, down);
}

void pushChar(int character)
{
	handleCharacter(character);
}

}

// Keyboard functions
jstring getKeyName(int_t key)
{
	// The same LWJGL-code -> display-name table the PS2/Wii/3DS ports use;
	// GLFW has glfwGetKeyName, but it only labels printable keys and answers
	// in the active keyboard layout, so it cannot back this LWJGL-parity API.
	// Same fallback shape as the console ports, so the Controls screen prints
	// the same thing everywhere.
	if (const char *name = lwjglKeyDisplayName(key))
		return name;
	return "KEY " + std::to_string(key);
}

// Event handling
static bool allow_repeat_events = false;

bool next()
{
	detail::flushRetained();

	if (detail::event_queue.empty())
		return false;

	if (!allow_repeat_events)
	{
		while (1)
		{
			if (detail::event_queue.empty())
				return false;
			if (detail::event_queue.front().repeat)
				detail::event_queue.pop();
			else
				break;
		}
	}

	if (detail::event_queue.empty())
		return false;

	detail::event_current = detail::event_queue.front();
	detail::event_queue.pop();
	return true;
}

void clearEvents()
{
	detail::flushRetained();
	while (!detail::event_queue.empty())
		detail::event_queue.pop();
	detail::has_retained_event = false;
	detail::event_current = {};
}

void enableRepeatEvents(bool repeat)
{
	allow_repeat_events = repeat;
}

bool areRepeatEventsEnabled()
{
	return allow_repeat_events;
}

char_t getEventCharacter()
{
	return detail::event_current.character;
}

int_t getEventKey()
{
	return detail::event_current.key;
}

bool getEventKeyState()
{
	return detail::event_current.down;
}

// Polling
bool isKeyDown(int_t key)
{
	const int_t glfw_key = detail::lwjglKeyToGLFW(key);
	if (glfw_key == GLFW_KEY_UNKNOWN)
		return false;
	return glfwGetKey(GLContext::detail::getWindow(), glfw_key) == GLFW_PRESS;
}


}
}
