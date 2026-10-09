#pragma once

#include "GuiScreen.h"
#include "MicrosoftAuthFlow.h"

// net.minecraft.src.GuiMicrosoftLogin -- the account screen reached from
// the multiplayer list's Account corner button. It runs the device-code
// flow (MicrosoftAuthFlow) on its own worker thread and shows the user code
// + link until Microsoft answers; no browser is embedded anywhere, which is
// exactly what makes the flow usable on every platform that has the
// transport (desktop with a client id, 3DS with a client id).
class GuiMicrosoftLogin : public GuiScreen
{
public:
	explicit GuiMicrosoftLogin(GuiScreen *parent);
	~GuiMicrosoftLogin() override;

	void initGui() override;
	void updateScreen() override;
	void drawScreen(int_t mouseX, int_t mouseY, float_t partialTick) override;

protected:
	void actionPerformed(GuiButton *button) override;
	void keyTyped(char_t c, int_t key) override;

private:
	void rebuildControls();
	void stopFlow();
	void drawWrapped(const std::string &text, int_t x, int_t y, int_t maxWidth, int_t color);

	GuiScreen *parentGui;
	MicrosoftAuthFlow *flow = nullptr;
	MicrosoftAuthFlow::Snapshot lastSnapshot;
	std::string failureText;
	bool flowRunning = false;
	bool succeeded = false;
};
