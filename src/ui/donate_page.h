#pragma once
#include <windows.h>

// UI worker only. Last hub page: says the mod is free and shows the author's Afdian card
// (QR code + link). Donating is optional; nothing in the mod depends on it.
namespace DonatePage {
HWND CreatePage(HWND parent);
void Tick();
void Shutdown();
}
