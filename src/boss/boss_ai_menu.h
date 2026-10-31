#pragma once
#include <windows.h>

// UI worker only; snapshots and requests never access engine objects here.
namespace BossAIMenu {
void Initialize();
void Tick();
void Show();
HWND CreatePage(HWND parent);
void Shutdown();
}
