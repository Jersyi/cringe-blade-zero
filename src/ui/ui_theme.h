#pragma once
#include <windows.h>
#include <cstdint>

// UI worker only. Styles native controls without replacing their window class,
// input handling, checked state, accessibility provider, fonts, or geometry.
namespace UiTheme {
void Initialize();
void Apply(HWND root);
HBRUSH Background();
HBRUSH PanelBrush();
HBRUSH BaseBrush();
COLORREF Text();
COLORREF MutedText();
COLORREF Accent();
COLORREF AccentBlue();
COLORREF Panel();
COLORREF Border();
COLORREF Selection();
// Window chrome (sidebar/header) behind the page surface returned by Background().
COLORREF Base();
COLORREF Surface();
COLORREF Danger();
COLORREF Mix(COLORREF a, COLORREF b, int percent);
bool HighContrast();

// Shared fonts, cached per DPI and owned by UiTheme; callers never delete them.
// Title and Heading use the game's title typeface (embedded subset) when it is
// available; GetFontFor falls back to Strong when the text has a missing glyph.
enum class Font { Body, Strong, Small, Title, Icon, Heading };
HFONT GetFont(Font role, UINT dpi);
HFONT GetFont(Font role, HWND window);
HFONT GetFontFor(Font role, UINT dpi, const wchar_t* text, int length = -1);
// Visual role of a page control. Section statics draw a heading with a divider,
// Hint statics use muted text, Card statics draw a rounded status panel and
// Primary buttons use the accent fill (BS_DEFPUSHBUTTON implies Primary).
enum class Role { None, Hint, Section, Card, Primary };
void SetRole(HWND control, Role role);
Role GetRole(HWND control);
// Closes an open custom drop-down list; true when one was open (Esc goes to it first).
bool DismissPopup();
bool PopupActive();

struct AnimationStats {
    uint32_t active_controls = 0, active_timers = 0;
    uint64_t started = 0, completed = 0, cancelled = 0;
    bool reduced_motion = false;
};
// Worker-thread diagnostic only. No engine/game thread work or global timer.
AnimationStats GetAnimationStats();
void Shutdown();
}
