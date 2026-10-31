#pragma once
#include "motian_grab.h"
#include <windows.h>
namespace MotianGrabMenu {
// Worker-thread Win32 UI only. Class name: PBZMotianGrabWindow.
void Initialize(MotianGrab::Logger logger);
HWND CreatePage(HWND parent);
void ToggleWindow();
void Tick();
void Shutdown();
}
