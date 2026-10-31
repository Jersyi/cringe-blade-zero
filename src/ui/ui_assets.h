#pragma once
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

// UI worker only, after UiTheme::Initialize (GDI+ running). Images are the game's
// own UI textures (button prompts, menu chrome) plus the hub avatar, embedded as
// PNG and decoded on first use. TitleFace is a private subset of the game's
// title typeface; it covers the dictionary strings only, so callers check Covers.
namespace UiAssets {
enum class Id : int {
    PsCross, PsCircle, PsSquare, PsTriangle, PsL1, PsR1, PsL2, PsR2, PsL3, PsR3, PsOptions,
    PsUp, PsDown, PsLeft, PsRight, PsTouch, PsSwipeLeft, PsSwipeRight, PsSwipeUp, PsSwipeDown,
    XbA, XbB, XbX, XbY, XbLB, XbRB, XbLT, XbRT, XbL3, XbR3, XbMenu, XbUp, XbDown, XbLeft, XbRight,
    SelectedBar, TabSelected, InkBar, Line, GoldArrow, Hook, Frame, SelectArrow, Avatar,
    GrinSheet, // 8x8 grid of 112 px banner face frames, 61 used
    FireSheet, // 6x3 grid of 128 px painted flame frames (world map fire, tinted red)
    Count
};
Gdiplus::Bitmap* Bitmap(Id id);
SIZE Size(Id id);
// Width that keeps the image's aspect at the given height.
int WidthAt(Id id, int height);
void Draw(Gdiplus::Graphics& graphics, Id id, const Gdiplus::RectF& rect, float opacity = 1.f, bool mirror = false);
// Stretches only the middle of a horizontal brush bar; the end caps keep their aspect.
void DrawBar(Gdiplus::Graphics& graphics, Id id, const Gdiplus::RectF& rect, float opacity = 1.f);
// Circular crop with a thin ring.
void DrawAvatar(Gdiplus::Graphics& graphics, const Gdiplus::RectF& rect, COLORREF ring, float ring_width);
const wchar_t* TitleFace();
// True when the font selected into dc has a glyph for every character.
bool Covers(HDC dc, const wchar_t* text, int length);
void Shutdown();
}
