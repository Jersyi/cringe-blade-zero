#pragma once
#include <windows.h>
#include <functional>
#include <string>

// UI worker only, except Hint and Bound (any thread). Custom chords for debug-menu
// actions. The bind control is a capture box: it never inserts characters. Every
// editable action has three bindings, keyboard, Xbox and PlayStation. The keyboard
// one always fires; of the pad ones only the set of the pad in use ([Hotkeys]
// PadGlyphs: auto / xbox / ps; auto = the pad behind the latest press, else the
// connected one). Prompts (Get, Hint) follow the device used last: the keyboard set
// after a key or mouse press, the pad's after a press on it. [Hotkeys] keys are
// <Action> (keyboard), <Action>.Xbox and <Action>.PS; each value is kb: / xb: / ps:
// plus tokens joined by '+', and a value without a prefix is a chord of that key's
// device ("F10", "Ctrl + F10", "Y + B"). Bindings missing from the ini are written
// with their defaults (Layout=2; an ini from the one-binding layout is migrated).
// ToggleHub (the panel, default F10) falls back to F10 when empty or unreadable.
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
// The chord prompts show: the set of the device used last, another set when that one is unbound.
Chord Get(Action action);
// One binding: device Keyboard, Xbox or PlayStation.
Chord Get(Action action, Device device);
// Any thread: some binding is set.
bool Bound(Action action);
// Any thread: Get(action) as text in the UI language ("G", "Y + B", "三角 + 圈").
std::wstring Hint(Action action);
// The pad set that fires now (Xbox or PlayStation).
Device ActivePad();
// The device prompts follow: Keyboard, or the active pad.
Device InputDevice();
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
