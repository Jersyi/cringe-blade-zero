#pragma once
#include <windows.h>
#include <functional>
#include <string>

// UI worker only. Custom chords for debug-menu actions. The bind control is a
// capture box: it never inserts characters. Keyboard, Xbox and PlayStation
// chords can be combined. A pad chord is a set of buttons that fires on either
// pad; [Hotkeys] PadGlyphs (auto / xbox / ps) picks the glyphs, auto = the pad
// in hand, or the recorded device when no pad is connected. Each [Hotkeys] value is
// kb: / xb: / ps: plus tokens joined by '+'; a value without a prefix is a keyboard
// chord ("F10", "Ctrl + F10"). ToggleHub (the panel, default F10) is written to the
// ini on first start and falls back to F10 when empty or unreadable.
namespace UiHotkeys {
enum class Action : int {
    ToggleHub = 0,
    ShowBoss,
    ShowAi,
    ShowCharacter,
    ShowNpc,
    ShowAir,
    ShowGrab,
    ShowGroom,
    ShowWeapons,
    HairMode,
    GpuBounds,
    ForceGroom,
    OutfitUnlock,
    AdaptiveTriggers,
    GrabRelease,
    WeaponSwap,
    GearUp,
    GearDown,
    NpcSkill,
    Count
};
enum class Device : unsigned char { Keyboard, Xbox, PlayStation };
struct Chord {
    Device device = Device::Keyboard;
    unsigned modifiers = 0;
    unsigned key = 0;
    unsigned buttons = 0;
    bool empty() const { return key == 0 && buttons == 0; }
};
void Initialize(const wchar_t* config_file);
void Bind(Action action, std::function<void()> handler);
void Tick();
bool Capturing();
HWND CreatePage(HWND parent);
Chord Get(Action action);
bool Bound(Action action);
std::wstring Plain(const Chord& chord);
// Draws keycaps or pad glyphs. Returns the x position after the last glyph.
int Draw(HDC dc, int x, int y, int height, const Chord& chord, int limit_x);
// One keycap in the game's keyboard-prompt style. Returns the right edge.
int DrawKey(HDC dc, int x, int y, int height, const wchar_t* caption);
// Game thread. kind 1 = DualSense / Edge, 2 = DualShock 4. The report includes its id byte.
void NoteSonyReport(int kind, const unsigned char* data, int size);
// Weapon HID thread. Ordered DualSense reports, preferred while that reader is live.
void NoteDualSenseTouch(const unsigned char* data, int size);
// While true, this module closes its own Sony handles so the weapon reader can own the device.
void YieldSony(bool yield);
// Offline tests only, before the first Tick: stands in for XInputGetState.
using PadStateReader = DWORD(WINAPI*)(DWORD user, void* state);
void SetXInputForTest(PadStateReader reader);
}
