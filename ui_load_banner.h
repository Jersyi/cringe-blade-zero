#pragma once

// UI worker only. No engine hooks, process access, timers or input injection.
namespace UiLoadBanner {
using Logger=void(*)(const char*);
void Initialize(Logger logger=nullptr);
void ShowSuccess();
void Tick();
bool Active();
void Shutdown();
}
