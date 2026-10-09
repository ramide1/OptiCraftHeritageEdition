#pragma once

#include <stdexcept>
#include <string>

#include <GLFW/glfw3.h>

// GLFW-flavoured replacement for the old SDLException: same role (turn a
// failed platform call into a runtime_error carrying the library's own
// description), backed by glfwGetError.
class GlfwException : public std::runtime_error
{
public:
	GlfwException() : std::runtime_error(describe())
	{
	}

private:
	static std::string describe()
	{
		const char *description = nullptr;
		glfwGetError(&description);
		return description != nullptr ? description : "unknown GLFW error";
	}
};
