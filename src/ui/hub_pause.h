#pragma once

// Pauses the game while the hub covers gameplay. The UI worker reports the hub
// state; the game thread pauses the current world with
// GameplayStatics::SetGamePaused and only ever releases a pause it set itself
// (a pause menu, cutscene or boss-entry pause that was already active is left
// alone). [Hub] PauseGame in PBZDebugMenu.ini, on by default.
namespace HubPause {
using Logger = void (*)(const char*);
void Initialize(Logger logger);
bool Enabled();
void SetEnabled(bool enabled); // UI worker; persisted
void Sync(bool open);          // UI worker, every hub tick
// Game thread, after the other features. hold_off: a flow that needs the world
// running (boss rush travel / spawn) is in progress; no pause is taken or kept.
void OnGameThread(bool hold_off);
bool Holding();                // a pause set by the hub is active
}
