#pragma once

#include <string>
#include <set>

#if !defined(PS2_PLATFORM) && !defined(WII_PLATFORM) && !defined(CTR_PLATFORM)
#include <GLFW/glfw3.h>
#include "glad/glad.h"
#endif

namespace lwjgl
{
namespace GLContext
{

// Detail implementation
namespace detail
{

// GL capabilities
struct GLCapabilities
{
private:
	std::set<std::string> caps;

public:
	void add(const std::string &cap)
	{
		caps.insert(cap);
	}

	bool operator[](const std::string &cap) const
	{
		return caps.find(cap) != caps.end();
	}
};

// Context singletons (desktop only; consoles own the framebuffer directly).
// GLFW folds the context into the window, so there is no separate context
// handle to hand out the way SDL_GLContext did.
#if !defined(PS2_PLATFORM) && !defined(WII_PLATFORM) && !defined(CTR_PLATFORM)
GLFWwindow *getWindow();
#endif

}

// Context functions
// Must be called before instantiate(); requested sample count is clamped to SDL-supported values.
void setRequestedSamples(int samples);
int getRequestedSamples();
void instantiate();
const detail::GLCapabilities &getCapabilities();

}
}
