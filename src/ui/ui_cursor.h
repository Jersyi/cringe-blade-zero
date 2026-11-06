#pragma once
#include <windows.h>

// UI worker arms the gate. The game thread applies it after UGameEngine::Tick.
// While the hub is open the game window stays foreground, so Unreal keeps hiding
// the cursor, recentering it, and reading raw mouse look. The gate stops that
// without taking the game window out of the foreground.
namespace UiCursor {
// Logs the game window's activation changes (UI_FOCUS lines).
void SetLogger(void (*log)(const char*));
void Sync(bool open);
void AfterTick();
// Click diagnostics (HUB_CLICK lines, the first 40 presses of a session; no behaviour change). The hub is a child of
// the game window on another thread, so a click on it waits for the game thread to answer the parent notifications.
// UI worker: each hub tick while it is shown (hub = its root window); every dispatched message's time.
void ProbeClicks(HWND hub);
void NoteDispatch(HWND hub, const MSG& message, double ms);
}
