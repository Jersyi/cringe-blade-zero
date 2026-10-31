#pragma once
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

// UI worker only, after UiTheme::Initialize (GDI+ running). The game's HUD icon
// and display name for a weapon ID as the backpack stores it (MainWeapons /
// ShaqiWeapons row names such as "BaiShanShuangJian"); see
// weapon_icons.generated.h. Icons are decoded on first use.
namespace WeaponIcons {
struct Info {
    const wchar_t* name = nullptr; // Current UI language; nullptr for an unknown ID.
    int image = -1;
};
Info Find(const wchar_t* id, bool shaqi);
// Aspect-fit and centred inside box.
void Draw(Gdiplus::Graphics& graphics, int image, const Gdiplus::RectF& box, float opacity = 1.f);
void Shutdown();
}
