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
// with their defaults (Layout=3; an ini from the one-binding layout is migrated, one of Layout=2 gets the panel's
// pad chords). ToggleHub (the panel: F10, View + Menu, Create + Options) falls back to F10 when the keyboard one is
// empty or unreadable. Pad chords fire while a text box has the focus; keyboard chords do not.
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
// Any thread: a binding of the action is held down as of the worker's latest poll (in the game, no text box or capture
// up), as it fires: the keyboard chord, or the active pad's.
bool Held(Action action);
// Any thread: Get(action) as text in the UI language ("G", "Y + B", "三角 + 圈").
std::wstring Hint(Action action);
// Any thread: the pad buttons held as of the worker's latest poll (Xbox and PlayStation presses on one layout: bits
// 0..15 = Up, Down, Left, Right, Menu, View, L3, R3, LB, RB, A, B, X, Y, LT, RT), whether the game window has the focus
// with no text box or capture up, and whether the pad in use is a PlayStation one.
// `stick`: the left stick pushed one way (one bit at a time, by its main axis: 1 up / forward, 2 down / back, 4 left,
// 8 right; 0 near the middle), on either pad.
struct PadLevel { unsigned buttons = 0; bool in_game = false; bool ps = false; ULONGLONG at = 0; unsigned stick = 0; };
PadLevel CurrentPad();
// The pad set that fires now (Xbox or PlayStation).
Device ActivePad();
// The device prompts follow: Keyboard, or the active pad.
Device InputDevice();
std::wstring Plain(const Chord& chord);
// Draws keycaps or pad glyphs. Returns the x position after the last glyph.
int Draw(HDC dc, int x, int y, int height, const Chord& chord, int limit_x);
// One keycap in the game's keyboard-prompt style. Returns the right edge.
int DrawKey(HDC dc, int x, int y, int height, const wchar_t* caption);
// How far Draw / DrawKey / DrawStick would go from x (Draw: including its gap after the last glyph).
int Width(HDC dc, int height, const Chord& chord);
int KeyWidth(HDC dc, int height, const wchar_t* caption);
int StickWidth(int height, bool ps);
// The left stick pushed one way (0 up, 1 down, 2 left, 3 right): the pad's L3 prompt icon and an arrow. Returns the
// right edge.
int DrawStick(HDC dc, int x, int y, int height, int way, bool ps);
// Game thread. kind 1 = DualSense / Edge, 2 = DualShock 4. The report includes its id byte.
void NoteSonyReport(int kind, const unsigned char* data, int size);
// Weapon HID thread. Ordered DualSense reports, preferred while that reader is live.
void NoteDualSenseTouch(const unsigned char* data, int size);
// While true, this module closes its own Sony handles so the weapon reader can own the device.
void YieldSony(bool yield);
// PadButton: pad buttons in the layout of PadLevel::buttons and Chord::buttons.
namespace PadButton {
constexpr unsigned Up = 1u << 0, Down = 1u << 1, Left = 1u << 2, Right = 1u << 3, Menu = 1u << 4, View = 1u << 5;
constexpr unsigned L3 = 1u << 6, R3 = 1u << 7, LB = 1u << 8, RB = 1u << 9, A = 1u << 10, B = 1u << 11, X = 1u << 12, Y = 1u << 13;
constexpr unsigned LT = 1u << 14, RT = 1u << 15;
}
// UI worker. While on (the panel is up and navigated with the pad), of the pad chords only ToggleHub fires; buttons
// held when it goes off stay out of pad chords until released.
void SetPadNavigation(bool on);
// UI worker: SetFocus for a pad focus move. A bind box that gets the focus this way is not armed.
void FocusFromPad(HWND control);
// UI worker: arms the bind box `control` for capture. False when it is not one.
bool ArmBox(HWND control);
// Offline tests only, before the first Tick: stands in for XInputGetState.
using PadStateReader = DWORD(WINAPI*)(DWORD user, void* state);
void SetXInputForTest(PadStateReader reader);
// PAD / HOTKEY lines (controllers found, pad in hand, hotkey fires) go here; set before Initialize.
void SetLogger(void (*log)(const char*));
}
