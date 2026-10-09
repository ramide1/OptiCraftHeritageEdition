#include "platform/Log.h"
#include "lwjgl/GLContext.h"

#include <iostream>
#include <stdexcept>
#include <csignal>

#include "external/GlfwException.h"
#include "pc/lwjgl/GlfwEvents.h"
#include "platform/RenderAPI.h"
#include "pc/render/PcRenderBackend.h"
#if defined(MC_WIN32)
#include "pc/render/d3d9/PcD3D9Context.h"
#endif

#include <GLFW/glfw3.h>

// #define MC_DEBUG_GL

#ifdef MC_DEBUG_GL
static void GLDebugMessageCallback(GLenum source, GLenum type, GLuint id,
                            GLenum severity, GLsizei length,
                            const GLchar *msg, const void *data)
{
    const char* _source;
    const char* _type;
    const char* _severity;

    switch (source) {
        case GL_DEBUG_SOURCE_API:
        _source = "API";
        break;

        case GL_DEBUG_SOURCE_WINDOW_SYSTEM:
        _source = "WINDOW SYSTEM";
        break;

        case GL_DEBUG_SOURCE_SHADER_COMPILER:
        _source = "SHADER COMPILER";
        break;

        case GL_DEBUG_SOURCE_THIRD_PARTY:
        _source = "THIRD PARTY";
        break;

        case GL_DEBUG_SOURCE_APPLICATION:
        _source = "APPLICATION";
        break;

        case GL_DEBUG_SOURCE_OTHER:
        _source = "UNKNOWN";
        break;

        default:
        _source = "UNKNOWN";
        break;
    }

    switch (type) {
        case GL_DEBUG_TYPE_ERROR:
        _type = "ERROR";
        break;

        case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR:
        _type = "DEPRECATED BEHAVIOR";
        break;

        case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR:
        _type = "UDEFINED BEHAVIOR";
        break;

        case GL_DEBUG_TYPE_PORTABILITY:
        _type = "PORTABILITY";
        break;

        case GL_DEBUG_TYPE_PERFORMANCE:
        _type = "PERFORMANCE";
        break;

        case GL_DEBUG_TYPE_OTHER:
        _type = "OTHER";
        break;

        case GL_DEBUG_TYPE_MARKER:
        _type = "MARKER";
        break;

        default:
        _type = "UNKNOWN";
        break;
    }

    switch (severity) {
        case GL_DEBUG_SEVERITY_HIGH:
        _severity = "HIGH";
        break;

        case GL_DEBUG_SEVERITY_MEDIUM:
        _severity = "MEDIUM";
        break;

        case GL_DEBUG_SEVERITY_LOW:
        _severity = "LOW";
        break;

        case GL_DEBUG_SEVERITY_NOTIFICATION:
        _severity = "NOTIFICATION";
        break;

        default:
        _severity = "UNKNOWN";
        break;
    }

    MC_LOG_INFO("game", "%d: %s of %s severity, raised from %s: %s\n",
            id, _type, _severity, _source, msg);
	std::raise(SIGINT);
}
#endif

namespace lwjgl
{
namespace GLContext
{

namespace
{
int requestedSamples = 0;

int sanitizeSampleCount(int samples)
{
	switch (samples)
	{
	case 2: case 4: case 8: case 16: return samples;
	default: return 0;
	}
}
}

// Detail implementation
namespace detail
{

// Context singleton
class GLContext
{
private:
	GLFWwindow *window = nullptr;
	GLCapabilities capabilities;

	void centerWindow()
	{
		// What SDL_WINDOWPOS_CENTERED used to do; GLFW places new windows
		// wherever the OS feels like.
		GLFWmonitor *monitor = glfwGetPrimaryMonitor();
		const GLFWvidmode *mode = glfwGetVideoMode(monitor);
		int monitor_x = 0, monitor_y = 0;
		glfwGetMonitorPos(monitor, &monitor_x, &monitor_y);
		glfwSetWindowPos(window, monitor_x + (mode->width - 854) / 2,
		                monitor_y + (mode->height - 480) / 2);
	}

public:
	GLContext()
	{
#if defined(MC_WIN32)
		if (pcRenderBackendGetRequested() == PcRenderBackendType::Direct3D9)
		{
			// No OpenGL context at all for the D3D9 backend — just a plain
			// native window whose HWND pcD3D9Initialize digs out.
			glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
			glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
			glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

			window = glfwCreateWindow(854, 480, "OptiCraft", nullptr, nullptr);
			if (window != nullptr && pcD3D9Initialize(window, requestedSamples))
			{
				requestedSamples = pcD3D9GetSamples();
				pcRenderBackendSetActive(PcRenderBackendType::Direct3D9);
				lwjgl::detail::installGlfwEventCallbacks(window);
				centerWindow();
				return;
			}

			pcD3D9Shutdown();
			if (window != nullptr)
			{
				glfwDestroyWindow(window);
				window = nullptr;
			}
		}
#endif

		pcRenderBackendSetActive(PcRenderBackendType::OpenGL);

		glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
		// Fixed-function renderer: 1.3 is the realistic floor (multisampled
		// pixel formats and the GL_SAMPLES/GL_SAMPLE_BUFFERS queries need
		// ARB_multisample-era core), and effectively what the old SDL 1.1
		// request produced in practice.
		glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 1);
		glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
		glfwWindowHint(GLFW_DEPTH_BITS, 24);
		glfwWindowHint(GLFW_DOUBLEBUFFER, GLFW_TRUE);
		glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
		glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
		// GLFW degrades the sample count itself when the requested one is
		// not available (it retries its pixel-format search without the
		// "soft" attributes), so the old manual fallback ladder is gone;
		// renderGetMaxSamples() reads back what was actually created.
		glfwWindowHint(GLFW_SAMPLES, requestedSamples);

#ifdef MC_DEBUG_GL
		glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);
#endif

		window = glfwCreateWindow(854, 480, "OptiCraft", nullptr, nullptr);
		if (window == nullptr)
			throw GlfwException();

		glfwMakeContextCurrent(window);

		if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)))
			throw std::runtime_error("Failed to load glad");

		glfwSwapInterval(0);

		const GLubyte *extensions = renderGetString(RenderStringQuery::Extensions);
		if (extensions != nullptr)
		{
			std::string cap;
			const char *extension_p = reinterpret_cast<const char *>(extensions);
			while (*extension_p != '\0')
			{
				if (*extension_p == ' ')
				{
					if (!cap.empty())
					{
						capabilities.add(cap);
						cap.clear();
					}
					while (*extension_p == ' ')
						++extension_p;
					continue;
				}

				cap.push_back(*extension_p++);
			}
			if (!cap.empty())
				capabilities.add(cap);
		}

		lwjgl::detail::installGlfwEventCallbacks(window);
		centerWindow();

#ifdef MC_DEBUG_GL
		glEnable(GL_DEBUG_OUTPUT);
		glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
		glDebugMessageCallback(GLDebugMessageCallback, nullptr);
#endif
	}

	~GLContext()
	{
#if defined(MC_WIN32)
		if (pcRenderBackendIsDirect3D9())
			pcD3D9Shutdown();
#endif
		if (window != nullptr)
			glfwDestroyWindow(window);
		// glfwTerminate is deliberately not called: the OS reclaims everything
		// at process exit and the static destructor ordering is not worth
		// betting the shutdown path on (SDL_Quit was never called either).
	}

	GLFWwindow *getWindow() const { return window; }
	const GLCapabilities &getCapabilities() const { return capabilities; }
};

// Context singletons
static GLContext &getContext()
{
	static GLContext context;
	return context;
}

GLFWwindow *getWindow()
{
	return getContext().getWindow();
}

}

// GL capabilities
void setRequestedSamples(int samples)
{
	requestedSamples = sanitizeSampleCount(samples);
}

int getRequestedSamples()
{
	return requestedSamples;
}

void instantiate()
{
	detail::getContext();
	if (pcRenderBackendIsDirect3D9())
		return;
	glfwMakeContextCurrent(detail::getWindow());
}

const detail::GLCapabilities &getCapabilities()
{
	return detail::getContext().getCapabilities();
}

}
}
