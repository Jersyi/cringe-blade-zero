#pragma once
#include <windows.h>

// Native worker-thread UI. Requests contain plain values only; engine calls
// are made by BossRush on the existing game-thread hook.
namespace BossMenu {
void Initialize();
void Tick();
void Show();
void ShowCharacters();
HWND CreatePage(HWND parent);
HWND CreateCharacterPage(HWND parent);
void Toggle();
void Shutdown();
}
