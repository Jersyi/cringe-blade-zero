#pragma once
#include <windows.h>

// Native UI for the existing DLL worker. Hub routing/CreatePage run on that
// worker; standalone Show queues a flag. Snapshot reads only, no UObject calls.
namespace NpcMenu {
using Logger = void (*)(const char*);
void Initialize(Logger logger = nullptr);
HWND CreatePage(HWND parent);
void Tick();
void Show();
void Shutdown();
}
