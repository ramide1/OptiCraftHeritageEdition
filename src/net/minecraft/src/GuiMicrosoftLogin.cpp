#include "GuiMicrosoftLogin.h"

#include "FontRenderer.h"
#include "GuiButton.h"
#include "GuiMainMenu.h"
#include "MicrosoftAccount.h"
#include "Minecraft.h"
#include "StringTranslate.h"
#include "UiStrings.h"
#include "pc/lwjgl/Keyboard.h"

GuiMicrosoftLogin::GuiMicrosoftLogin(GuiScreen *parent)
	: parentGui(parent)
{
}

GuiMicrosoftLogin::~GuiMicrosoftLogin()
{
	stopFlow();
}

void GuiMicrosoftLogin::initGui()
{
	rebuildControls();
}

void GuiMicrosoftLogin::stopFlow()
{
	if (flow != nullptr)
	{
		flow->cancel();
		delete flow; // the worker joins in its own destructor
		flow = nullptr;
	}
	flowRunning = false;
}

void GuiMicrosoftLogin::rebuildControls()
{
	// The button set follows the screen's state machine; clearControlList()
	// owns deletion and mouse capture, so re-entrant rebuilds stay clean.
	clearControlList();
	StringTranslate *translate = StringTranslate::getInstance();
	if (flowRunning)
	{
		controlList.push_back(new GuiButton(1, width / 2 - 100, height / 4 + 132,
		                                   translate->translateKey("gui.cancel")));
		return;
	}
	if (!failureText.empty())
	{
		controlList.push_back(new GuiButton(0, width / 2 - 100, height / 4 + 108, uiText("Retry")));
		controlList.push_back(new GuiButton(1, width / 2 - 100, height / 4 + 132,
		                                   translate->translateKey("gui.done")));
		return;
	}
	if (succeeded)
	{
		controlList.push_back(new GuiButton(1, width / 2 - 100, height / 4 + 108,
		                                   translate->translateKey("gui.done")));
		return;
	}
	if (MicrosoftAccounts::hasAccount())
	{
		controlList.push_back(new GuiButton(2, width / 2 - 100, height / 4 + 108, uiText("Logout")));
		controlList.push_back(new GuiButton(1, width / 2 - 100, height / 4 + 132,
		                                   translate->translateKey("gui.done")));
		return;
	}
	controlList.push_back(new GuiButton(0, width / 2 - 100, height / 4 + 108, uiText("Login")));
	controlList.push_back(new GuiButton(1, width / 2 - 100, height / 4 + 132,
	                                   translate->translateKey("gui.done")));
}

void GuiMicrosoftLogin::actionPerformed(GuiButton *button)
{
	if (button == nullptr || !button->enabled)
		return;
	if (button->id == 0) // Login / Retry: start the device-code flow
	{
		stopFlow();
		failureText.clear();
		succeeded = false;
		lastSnapshot = MicrosoftAuthFlow::Snapshot();
		flow = new MicrosoftAuthFlow();
		flowRunning = true;
		rebuildControls();
		flow->start();
	}
	else if (button->id == 1) // Back / Cancel
	{
		stopFlow();
		if (parentGui != nullptr)
			mc->displayGuiScreen(parentGui);
		else
			mc->displayGuiScreen(new GuiMainMenu());
	}
	else if (button->id == 2) // Logout
	{
		MicrosoftAccounts::clear();
		succeeded = false;
		failureText.clear();
		rebuildControls();
	}
}

void GuiMicrosoftLogin::keyTyped(char_t c, int_t key)
{
	if (key == lwjgl::Keyboard::KEY_ESCAPE)
	{
		stopFlow();
		if (parentGui != nullptr)
			mc->displayGuiScreen(parentGui);
		else
			mc->displayGuiScreen(new GuiMainMenu());
		return;
	}
	(void)c;
}

void GuiMicrosoftLogin::updateScreen()
{
	if (flow == nullptr)
		return;
	MicrosoftAuthFlow::Snapshot snapshot;
	while (flow->takeSnapshot(snapshot))
		lastSnapshot = snapshot;
	if (!lastSnapshot.finished)
		return;
	flowRunning = false;
	if (lastSnapshot.state == MicrosoftAuthFlow::State::Done)
	{
		succeeded = true;
		failureText.clear();
	}
	else
	{
		failureText = lastSnapshot.error;
	}
	stopFlow();
	rebuildControls();
}

void GuiMicrosoftLogin::drawWrapped(const std::string &text, int_t x, int_t y, int_t maxWidth, int_t color)
{
	std::size_t position = 0;
	int_t lineY = y;
	while (position < text.size())
	{
		std::size_t end = position;
		std::size_t lastSpace = std::string::npos;
		while (end < text.size() && text[end] != '\n')
		{
			if (text[end] == ' ')
				lastSpace = end;
			end++;
			if (lastSpace != std::string::npos &&
			    fontRenderer->getStringWidth(text.substr(position, end - position)) > maxWidth)
			{
				end = lastSpace;
				break;
			}
		}
		drawString(fontRenderer, text.substr(position, end - position), x, lineY, color);
		lineY += 12;
		if (end < text.size() && text[end] == '\n')
			end++;
		position = end;
	}
}

void GuiMicrosoftLogin::drawScreen(int_t mouseX, int_t mouseY, float_t partialTick)
{
	drawDefaultBackground();
	const int_t centerX = width / 2;
	int_t y = height / 4 - 40;
	drawCenteredString(fontRenderer, uiText("Microsoft Account"), centerX, y, 0xffffff);
	y += 26;

	if (flowRunning)
	{
		if (lastSnapshot.state == MicrosoftAuthFlow::State::WaitingForUser)
		{
			drawCenteredString(fontRenderer, uiText("Open this address on any device") + ":",
			                   centerX, y, 0xa0a0a0);
			y += 14;
			drawCenteredString(fontRenderer, lastSnapshot.verificationUrl, centerX, y, 0xffffff);
			y += 22;
			drawCenteredString(fontRenderer, uiText("and enter this code") + ":",
			                   centerX, y, 0xa0a0a0);
			y += 14;
			// The whole point of the screen: the code is what the player
			// copies to another device, so it gets its own bright line.
			drawCenteredString(fontRenderer, lastSnapshot.userCode, centerX, y, 0x55ff55);
		}
		else if (lastSnapshot.state == MicrosoftAuthFlow::State::ExchangingTokens)
		{
			drawCenteredString(fontRenderer, uiText("Signing in to Xbox and Minecraft") + "...",
			                   centerX, y, 0xffffff);
		}
		else
		{
			drawCenteredString(fontRenderer, uiText("Contacting Microsoft") + "...",
			                   centerX, y, 0xffffff);
		}
	}
	else if (succeeded)
	{
		drawCenteredString(fontRenderer, uiText("Login complete") + ":", centerX, y, 0x55ff55);
		y += 14;
		if (MicrosoftAccounts::hasAccount())
			drawCenteredString(fontRenderer, MicrosoftAccounts::account().username, centerX, y, 0xffffff);
	}
	else if (!failureText.empty())
	{
		drawWrapped(failureText, centerX - 140, y, 280, 0xff5555);
	}
	else if (MicrosoftAccounts::hasAccount())
	{
		drawCenteredString(fontRenderer, uiText("Logged in as") + ":", centerX, y, 0xa0a0a0);
		y += 14;
		drawCenteredString(fontRenderer, MicrosoftAccounts::account().username, centerX, y, 0xffffff);
	}
	else
	{
		drawWrapped(uiText("Sign in with a Microsoft account to use your profile "
		                   "name and join online-mode servers."),
		           centerX - 140, y, 280, 0xa0a0a0);
	}

	GuiScreen::drawScreen(mouseX, mouseY, partialTick);
}
