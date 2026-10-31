#pragma once
#include <windows.h>
namespace WeaponPresetsMenu {
HWND CreatePage(HWND parent);
void Tick();
void Show();
void Shutdown();
}
