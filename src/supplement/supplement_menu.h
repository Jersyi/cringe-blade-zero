#pragma once
#include <windows.h>

// UI worker only. Hub page for the debug supplement (DebugSupplement): our own cards over its catalog
// (supplement_layout.h). Registered only when the supplement container is present.
namespace SupplementMenu {
HWND CreatePage(HWND parent);
void Tick();
void Shutdown();
}
