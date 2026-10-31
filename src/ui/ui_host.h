#pragma once
#include <windows.h>

// UI worker only. Parents this DLL's windows to the game window so they stay
// inside the client, hide with the game, and do not take a taskbar button.
// The game window belongs to another thread. Callers may read the cached
// client origin; they must not SendMessage the game window from a window proc.
namespace UiHost {
void Tick();
HWND Game();
bool InGame();
bool Attach(HWND window);
void Detach(HWND window);
POINT ClientOrigin();
SIZE ClientSize();
RECT ClientScreen();
bool FocusInside(HWND root);
}
