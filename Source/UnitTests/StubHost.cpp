// Copyright 2014 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

// Stub implementation of the Host_* callbacks for tests. These implementations
// do nothing except return default values when required.

#include <memory>
#include <string>

#include "Core/Host.h"

void Host_NotifyMapLoaded()
{
}
void Host_RefreshDSPDebuggerWindow()
{
}
void Host_Message(HostMessageID)
{
}
void Host_UpdateTitle(const std::string&)
{
}
void Host_UpdateDisasmDialog()
{
}
void Host_UpdateMainFrame()
{
}
void Host_RequestRenderWindowSize(int, int)
{
}
bool Host_UIBlocksControllerState()
{
  return false;
}
bool Host_RendererHasFocus()
{
  return false;
}
bool Host_RendererIsFullscreen()
{
  return false;
}
void Host_YieldToUI()
{
}
void Host_TitleChanged()
{
}
void Host_LowerWindow()
{
}
void Host_Exit()
{
}
void Host_PlaybackSeek()
{
}
void Host_Fullscreen()
{
}
