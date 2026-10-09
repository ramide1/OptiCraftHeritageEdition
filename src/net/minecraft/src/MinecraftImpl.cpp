#include "platform/Log.h"
#include "MinecraftImpl.h"

#include <iostream>

#include "PanelCrashReport.h"
#include "UnexpectedThrowable.h"

MinecraftImpl::MinecraftImpl(void *component, void *canvas, void *minecraftapplet,
                             int_t width, int_t height, bool fullscreen, void *frame)
    : Minecraft(width, height, fullscreen),
      mcFrame(frame)
{
}

void MinecraftImpl::displayUnexpectedThrowable(UnexpectedThrowable *unexpectedthrowable)
{
    PanelCrashReport report(unexpectedthrowable);
    MC_LOG_ERROR("crash", "%s\n", report.getText().c_str());
    // The crash text is logged, and CrashHandler::Crash takes it from here
    // (a native MessageBox on Windows, stderr elsewhere);
    // the frame reference (mcFrame) would receive the panel in the AWT build.
}
