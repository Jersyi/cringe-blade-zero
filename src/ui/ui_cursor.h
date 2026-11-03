#pragma once

// UI worker arms the gate. The game thread applies it after UGameEngine::Tick.
// While the hub is open the game window stays foreground, so Unreal keeps hiding
// the cursor, recentering it, and reading raw mouse look. The gate stops that
// without taking the game window out of the foreground.
namespace UiCursor {
// Logs the game window's activation changes (UI_FOCUS lines).
void SetLogger(void (*log)(const char*));
void Sync(bool open);
void AfterTick();
}
