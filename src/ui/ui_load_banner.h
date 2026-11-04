#pragma once

// UI worker only. No engine hooks, process access, timers or input injection.
namespace UiLoadBanner {
using Logger=void(*)(const char*);
void Initialize(Logger logger=nullptr);
void ShowSuccess();
void ShowUnsupported();  // the game is not the build the tools pin: the same banner as a notice, face kept cold
void Tick();
bool Active();
bool Pending();          // shown once the game window can host it (gives up after 15 s)
void Shutdown();
}
