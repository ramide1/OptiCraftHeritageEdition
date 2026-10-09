#include "pc/lwjgl/GlfwKeymap.h"

#include <GLFW/glfw3.h>

namespace lwjgl
{
namespace Keyboard
{
namespace detail
{

namespace
{

// (GLFW key, LWJGL code) pairs. LWJGL codes are the DirectInput set the Key
// enum is built on; GLFW codes are the USB-derived set glfw3.h defines. The
// table covers every key GLFW can report that has an LWJGL counterpart; the
// J-specific (KANA, CONVERT, YEN, ...) and Mac-specific (FUNCTION, SECTION,
// CLEAR, POWER, SLEEP, ...) codes have no GLFW counterpart on the platforms
// the desktop target ships for.
struct KeyPair
{
	int glfw;
	int lwjgl;
};

constexpr KeyPair kKeyPairs[] = {
	{ GLFW_KEY_SPACE, KEY_SPACE },
	{ GLFW_KEY_APOSTROPHE, KEY_APOSTROPHE },
	{ GLFW_KEY_COMMA, KEY_COMMA },
	{ GLFW_KEY_MINUS, KEY_MINUS },
	{ GLFW_KEY_PERIOD, KEY_PERIOD },
	{ GLFW_KEY_SLASH, KEY_SLASH },
	{ GLFW_KEY_0, KEY_0 },
	{ GLFW_KEY_1, KEY_1 },
	{ GLFW_KEY_2, KEY_2 },
	{ GLFW_KEY_3, KEY_3 },
	{ GLFW_KEY_4, KEY_4 },
	{ GLFW_KEY_5, KEY_5 },
	{ GLFW_KEY_6, KEY_6 },
	{ GLFW_KEY_7, KEY_7 },
	{ GLFW_KEY_8, KEY_8 },
	{ GLFW_KEY_9, KEY_9 },
	{ GLFW_KEY_SEMICOLON, KEY_SEMICOLON },
	{ GLFW_KEY_EQUAL, KEY_EQUALS },
	{ GLFW_KEY_A, KEY_A },
	{ GLFW_KEY_B, KEY_B },
	{ GLFW_KEY_C, KEY_C },
	{ GLFW_KEY_D, KEY_D },
	{ GLFW_KEY_E, KEY_E },
	{ GLFW_KEY_F, KEY_F },
	{ GLFW_KEY_G, KEY_G },
	{ GLFW_KEY_H, KEY_H },
	{ GLFW_KEY_I, KEY_I },
	{ GLFW_KEY_J, KEY_J },
	{ GLFW_KEY_K, KEY_K },
	{ GLFW_KEY_L, KEY_L },
	{ GLFW_KEY_M, KEY_M },
	{ GLFW_KEY_N, KEY_N },
	{ GLFW_KEY_O, KEY_O },
	{ GLFW_KEY_P, KEY_P },
	{ GLFW_KEY_Q, KEY_Q },
	{ GLFW_KEY_R, KEY_R },
	{ GLFW_KEY_S, KEY_S },
	{ GLFW_KEY_T, KEY_T },
	{ GLFW_KEY_U, KEY_U },
	{ GLFW_KEY_V, KEY_V },
	{ GLFW_KEY_W, KEY_W },
	{ GLFW_KEY_X, KEY_X },
	{ GLFW_KEY_Y, KEY_Y },
	{ GLFW_KEY_Z, KEY_Z },
	{ GLFW_KEY_LEFT_BRACKET, KEY_LBRACKET },
	{ GLFW_KEY_BACKSLASH, KEY_BACKSLASH },
	{ GLFW_KEY_RIGHT_BRACKET, KEY_RBRACKET },
	{ GLFW_KEY_GRAVE_ACCENT, KEY_GRAVE },
	{ GLFW_KEY_ESCAPE, KEY_ESCAPE },
	{ GLFW_KEY_ENTER, KEY_RETURN },
	{ GLFW_KEY_TAB, KEY_TAB },
	{ GLFW_KEY_BACKSPACE, KEY_BACK },
	{ GLFW_KEY_INSERT, KEY_INSERT },
	{ GLFW_KEY_DELETE, KEY_DELETE },
	{ GLFW_KEY_RIGHT, KEY_RIGHT },
	{ GLFW_KEY_LEFT, KEY_LEFT },
	{ GLFW_KEY_DOWN, KEY_DOWN },
	{ GLFW_KEY_UP, KEY_UP },
	{ GLFW_KEY_PAGE_UP, KEY_PRIOR },
	{ GLFW_KEY_PAGE_DOWN, KEY_NEXT },
	{ GLFW_KEY_HOME, KEY_HOME },
	{ GLFW_KEY_END, KEY_END },
	{ GLFW_KEY_CAPS_LOCK, KEY_CAPITAL },
	{ GLFW_KEY_SCROLL_LOCK, KEY_SCROLL },
	{ GLFW_KEY_NUM_LOCK, KEY_NUMLOCK },
	{ GLFW_KEY_PRINT_SCREEN, KEY_SYSRQ },
	{ GLFW_KEY_PAUSE, KEY_PAUSE },
	{ GLFW_KEY_F1, KEY_F1 },
	{ GLFW_KEY_F2, KEY_F2 },
	{ GLFW_KEY_F3, KEY_F3 },
	{ GLFW_KEY_F4, KEY_F4 },
	{ GLFW_KEY_F5, KEY_F5 },
	{ GLFW_KEY_F6, KEY_F6 },
	{ GLFW_KEY_F7, KEY_F7 },
	{ GLFW_KEY_F8, KEY_F8 },
	{ GLFW_KEY_F9, KEY_F9 },
	{ GLFW_KEY_F10, KEY_F10 },
	{ GLFW_KEY_F11, KEY_F11 },
	{ GLFW_KEY_F12, KEY_F12 },
	{ GLFW_KEY_F13, KEY_F13 },
	{ GLFW_KEY_F14, KEY_F14 },
	{ GLFW_KEY_F15, KEY_F15 },
	{ GLFW_KEY_F16, KEY_F16 },
	{ GLFW_KEY_F17, KEY_F17 },
	{ GLFW_KEY_F18, KEY_F18 },
	{ GLFW_KEY_F19, KEY_F19 },
	{ GLFW_KEY_KP_0, KEY_NUMPAD0 },
	{ GLFW_KEY_KP_1, KEY_NUMPAD1 },
	{ GLFW_KEY_KP_2, KEY_NUMPAD2 },
	{ GLFW_KEY_KP_3, KEY_NUMPAD3 },
	{ GLFW_KEY_KP_4, KEY_NUMPAD4 },
	{ GLFW_KEY_KP_5, KEY_NUMPAD5 },
	{ GLFW_KEY_KP_6, KEY_NUMPAD6 },
	{ GLFW_KEY_KP_7, KEY_NUMPAD7 },
	{ GLFW_KEY_KP_8, KEY_NUMPAD8 },
	{ GLFW_KEY_KP_9, KEY_NUMPAD9 },
	{ GLFW_KEY_KP_DECIMAL, KEY_DECIMAL },
	{ GLFW_KEY_KP_DIVIDE, KEY_DIVIDE },
	{ GLFW_KEY_KP_MULTIPLY, KEY_MULTIPLY },
	{ GLFW_KEY_KP_SUBTRACT, KEY_SUBTRACT },
	{ GLFW_KEY_KP_ADD, KEY_ADD },
	{ GLFW_KEY_KP_ENTER, KEY_NUMPADENTER },
	{ GLFW_KEY_KP_EQUAL, KEY_NUMPADEQUALS },
	{ GLFW_KEY_LEFT_SHIFT, KEY_LSHIFT },
	{ GLFW_KEY_LEFT_CONTROL, KEY_LCONTROL },
	{ GLFW_KEY_LEFT_ALT, KEY_LMENU },
	{ GLFW_KEY_LEFT_SUPER, KEY_LMETA },
	{ GLFW_KEY_RIGHT_SHIFT, KEY_RSHIFT },
	{ GLFW_KEY_RIGHT_CONTROL, KEY_RCONTROL },
	{ GLFW_KEY_RIGHT_ALT, KEY_RMENU },
	{ GLFW_KEY_RIGHT_SUPER, KEY_RMETA },
	{ GLFW_KEY_MENU, KEY_APPS },
};

}

int glfwKeyToLWJGL(int glfwKey)
{
	for (const KeyPair &pair : kKeyPairs)
		if (pair.glfw == glfwKey)
			return pair.lwjgl;
	return KEY_NONE;
}

int_t lwjglKeyToGLFW(int_t lwjglKey)
{
	for (const KeyPair &pair : kKeyPairs)
		if (pair.lwjgl == lwjglKey)
			return pair.glfw;
	return GLFW_KEY_UNKNOWN;
}

}
}
}
