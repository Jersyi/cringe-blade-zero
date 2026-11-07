#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>

// What the game may see of a pad while the panel is up and right after it. Pure logic: the hooks that feed it are in
// game_pad_gate.cpp. Buttons are UiHotkeys::PadButton bits (bits 0..15 = Up, Down, Left, Right, Menu, View, L3, R3,
// LB, RB, A, B, X, Y, LT, RT).
//  - Panel up (blocked): the game reads a pad at rest (no buttons, sticks centred, triggers released).
//  - Buttons held while it was up stay hidden after it goes away, each until released (B that closed it does
//    nothing in the game).
//  - The panel chord (ToggleHub of that pad): its buttons are held back for up to kChordWaitMs after one of them goes
//    down. The whole chord down: hidden until released. A held-back button let go before that: shown pressed for
//    kReplayMs afterwards, so a quick tap still reaches the game. Held past the wait: shown from then on.
namespace PadGate {
constexpr ULONGLONG kChordWaitMs = 150, kReplayMs = 80;
constexpr BYTE kTriggerOn = 30;  // XINPUT_GAMEPAD_TRIGGER_THRESHOLD; also the DualSense analog L2 / R2

// One stream of one pad: an XInput user, a HID handle, a raw input device.
struct Gate {
    unsigned last = 0;        // buttons of the previous call
    unsigned stale = 0;       // held while blocked: hidden until released
    unsigned chord_down = 0;  // the whole chord was down: hidden until released
    unsigned waiting = 0;     // chord buttons held back, waiting for the rest
    ULONGLONG waiting_since = 0;
    unsigned replay = 0;      // held-back buttons let go early: shown pressed until replay_until
    ULONGLONG replay_until = 0;
    bool hid = false;         // the latest call hid something or showed a replay
};
struct View {
    unsigned buttons = 0;  // what the game may see
    bool neutral = false;  // a pad at rest: no buttons, sticks centred, triggers released
};
// held: the buttons down now. blocked: the panel is up. chord: this pad's panel chord (0: none).
View Filter(Gate& gate, unsigned held, bool blocked, unsigned chord, ULONGLONG now);

// XINPUT_GAMEPAD.
struct XPad { WORD buttons; BYTE left, right; SHORT lx, ly, rx, ry; };
static_assert(sizeof(XPad) == 12, "XINPUT_GAMEPAD layout");
unsigned FromXInput(WORD buttons, BYTE left, BYTE right);
WORD ToXInput(unsigned buttons);
// In place. True when the pad changed.
bool FilterXInput(Gate& gate, XPad& pad, bool blocked, unsigned chord, ULONGLONG now);

// DualSense / DualSense Edge input reports: USB 0x01 (64 bytes), Bluetooth 0x31 (78 bytes, CRC renewed) and the short
// Bluetooth 0x01 sent before the full mode (78-byte buffer or 10 bytes). Other reports: untouched, false.
bool DualSenseButtons(const uint8_t* report, size_t size, unsigned& held);
// In place. True when the report changed.
bool FilterDualSense(Gate& gate, uint8_t* report, size_t size, bool blocked, unsigned chord, ULONGLONG now);
}
