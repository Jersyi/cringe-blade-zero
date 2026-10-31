#include "weapon_icons.h"
#include "weapon_icons.generated.h"
#include "ui_i18n.h"
#include <shlwapi.h>
#include <algorithm>
#include <array>
#include <iterator>
#include <memory>
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "gdiplus.lib")

namespace WeaponIcons {
namespace {
constexpr size_t kCount = std::size(WeaponIconsData::kImages);
std::array<std::unique_ptr<Gdiplus::Bitmap>, kCount> bitmaps_;
std::array<bool, kCount> tried_{};

bool Same(const char* ascii, const wchar_t* wide) {
    for (; *ascii && *wide; ++ascii, ++wide) if (static_cast<wchar_t>(static_cast<unsigned char>(*ascii)) != *wide) return false;
    return !*ascii && !*wide;
}
std::unique_ptr<Gdiplus::Bitmap> Decode(const WeaponIconsData::Image& image) {
    IStream* stream = SHCreateMemStream(image.data, static_cast<UINT>(image.size));
    if (!stream) return nullptr;
    std::unique_ptr<Gdiplus::Bitmap> decoded(Gdiplus::Bitmap::FromStream(stream));
    stream->Release();
    if (!decoded || decoded->GetLastStatus() != Gdiplus::Ok) return nullptr;
    // Premultiplied copy, as in UiAssets: GDI+ draws PARGB without a per-call conversion.
    auto result = std::make_unique<Gdiplus::Bitmap>(static_cast<INT>(decoded->GetWidth()), static_cast<INT>(decoded->GetHeight()), PixelFormat32bppPARGB);
    if (result->GetLastStatus() != Gdiplus::Ok) return nullptr;
    Gdiplus::Graphics graphics(result.get());
    graphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    graphics.DrawImage(decoded.get(), 0, 0, static_cast<INT>(decoded->GetWidth()), static_cast<INT>(decoded->GetHeight()));
    return result;
}
Gdiplus::Bitmap* Bitmap(int image) {
    if (image < 0 || static_cast<size_t>(image) >= kCount) return nullptr;
    const size_t index = static_cast<size_t>(image);
    if (!tried_[index]) { tried_[index] = true; bitmaps_[index] = Decode(WeaponIconsData::kImages[index]); }
    return bitmaps_[index].get();
}
}

Info Find(const wchar_t* id, bool shaqi) {
    if (!id || !*id) return {};
    // Prefer the table of the slot; an ID can only appear in the other one if
    // the game moved a weapon between tables.
    const WeaponIconsData::Weapon* match = nullptr;
    for (const auto& weapon : WeaponIconsData::kWeapons) {
        if (!Same(weapon.id, id)) continue;
        if (weapon.shaqi == shaqi) { match = &weapon; break; }
        if (!match) match = &weapon;
    }
    if (!match) return {};
    const bool english = UiI18n::GetLanguage() == UiI18n::Language::English;
    return {english ? match->en : match->zh, match->image};
}
void Draw(Gdiplus::Graphics& graphics, int image, const Gdiplus::RectF& box, float opacity) {
    Gdiplus::Bitmap* bitmap = Bitmap(image);
    if (!bitmap || box.Width <= 0 || box.Height <= 0 || opacity <= 0) return;
    const float w = static_cast<float>(bitmap->GetWidth()), h = static_cast<float>(bitmap->GetHeight());
    const float scale = (std::min)(box.Width / w, box.Height / h);
    const Gdiplus::RectF target(box.X + (box.Width - w * scale) / 2, box.Y + (box.Height - h * scale) / 2, w * scale, h * scale);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    Gdiplus::ImageAttributes attributes;
    if (opacity < .999f) {
        Gdiplus::ColorMatrix matrix{};
        for (int i = 0; i < 5; ++i) matrix.m[i][i] = 1.f;
        matrix.m[3][3] = opacity;
        attributes.SetColorMatrix(&matrix);
    }
    graphics.DrawImage(bitmap, target, 0, 0, w, h, Gdiplus::UnitPixel, opacity < .999f ? &attributes : nullptr);
}
void Shutdown() {
    for (auto& bitmap : bitmaps_) bitmap.reset();
    tried_.fill(false);
}
}
