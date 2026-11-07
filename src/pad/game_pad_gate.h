#pragma once
#include <windows.h>

// Keeps the pads away from the game while the panel is up (PadGate rules: a pad at rest while it is up, buttons held
// then hidden until released, the panel chord held back), and logs how the game reads its pads (GAME_PAD lines).
//  - Xbox and other XInput pads: PBZ.exe's delay-import slot of XINPUT1_4!XInputGetState (Unreal), and the
//    XInputGetState / XInputGetStateEx (ordinal 100) that the game's SDL gets from GetProcAddress.
//  - DualSense: SDL hidapi reads of the DualSense handles (ReadFile / GetOverlappedResult), and HID raw input reports
//    the game reads (GetRawInputData).
// Only import entries of PBZ.exe change. Until Update gives the chords, nothing is held back (unsupported builds).
namespace GamePadGate {
void Install();                              // DllMain of PBZ.exe, after PadTriggers::Install
void SetLogger(void (*log)(const char*));    // worker, before Update
// UI worker, every tick: the panel covers the game; the ToggleHub chords of the Xbox and PlayStation sets
// (UiHotkeys::PadButton bits). Also re-applies the delay-import slot if the delay loader rewrote it.
void Update(bool blocked, unsigned xbox_chord, unsigned ps_chord);
}
