#pragma once
#include <windows.h>
#include <functional>

// Worker-thread UI only. Pages keep their existing request/snapshot boundary.
namespace DebugHub {
enum class Page { BossRush, BossAI, Character, Npc, AirSkills, MotianGrab, Groom, WeaponPresets, Hotkeys, Count };
using Factory = std::function<HWND(HWND)>;
void Initialize();
void Register(Page page, const wchar_t* title, Factory factory);
bool Enabled();
void ShowPage(Page page);
void Toggle();
void Hide();
void Tick();
void Shutdown();
void Attach(HWND child, HWND parent, int min_width, int min_height);
// A page reports the smallest size its current text fits in; the hub scrolls below it.
void SetMinimum(HWND page, int min_width, int min_height);
// Shows a short animated notice over the hub; false when the hub is hidden.
bool Notify(const wchar_t* text);
bool IsEmbedded(HWND child);
bool Visible();
// True while the hub is actually on screen. A child of the game window does not
// become the foreground window, so boss-rush pause uses this as well as focus.
bool ObscuresGameplay();
void RefreshChrome();
bool Animating();
// Game thread. One native debug-menu O, after the worker hid the hub and
// gameplay focus is back. Stays pending while the world is still paused.
bool TakeNativeMenuKey();
}
