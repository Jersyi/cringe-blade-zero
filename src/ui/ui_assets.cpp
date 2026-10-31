#include "ui_assets.h"
#include "ui_assets.generated.h"
#include <shlwapi.h>
#include <algorithm>
#include <array>
#include <iterator>
#include <memory>
#include <vector>
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "gdiplus.lib")

namespace UiAssets {
namespace {
constexpr int kCount = static_cast<int>(Id::Count);
static_assert(std::size(UiAssetsData::kEntries) == kCount, "ui_assets.generated.h does not match UiAssets::Id");
std::array<std::unique_ptr<Gdiplus::Bitmap>, kCount> bitmaps_;
std::array<bool, kCount> tried_{};
HANDLE font_ = nullptr;
bool font_tried_ = false;

std::unique_ptr<Gdiplus::Bitmap> Decode(const UiAssetsData::Entry& entry) {
    IStream* stream = SHCreateMemStream(entry.data, static_cast<UINT>(entry.size));
    if (!stream) return nullptr;
    std::unique_ptr<Gdiplus::Bitmap> decoded(Gdiplus::Bitmap::FromStream(stream));
    stream->Release();
    if (!decoded || decoded->GetLastStatus() != Gdiplus::Ok) return nullptr;
    // Premultiplied copy: GDI+ draws PARGB without a per-call conversion.
    auto result = std::make_unique<Gdiplus::Bitmap>(static_cast<INT>(decoded->GetWidth()), static_cast<INT>(decoded->GetHeight()), PixelFormat32bppPARGB);
    if (result->GetLastStatus() != Gdiplus::Ok) return nullptr;
    Gdiplus::Graphics graphics(result.get());
    graphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    graphics.DrawImage(decoded.get(), 0, 0, static_cast<INT>(decoded->GetWidth()), static_cast<INT>(decoded->GetHeight()));
    return result;
}
void Quality(Gdiplus::Graphics& graphics) {
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
}
Gdiplus::ImageAttributes* Fade(Gdiplus::ImageAttributes& attributes, float opacity) {
    if (opacity >= .999f) return nullptr;
    Gdiplus::ColorMatrix matrix{};
    for (int i = 0; i < 5; ++i) matrix.m[i][i] = 1.f;
    matrix.m[3][3] = (std::max)(0.f, opacity);
    attributes.SetColorMatrix(&matrix);
    return &attributes;
}
}
Gdiplus::Bitmap* Bitmap(Id id) {
    const int index = static_cast<int>(id);
    if (index < 0 || index >= kCount) return nullptr;
    if (!tried_[static_cast<size_t>(index)]) {
        tried_[static_cast<size_t>(index)] = true;
        bitmaps_[static_cast<size_t>(index)] = Decode(UiAssetsData::kEntries[index]);
    }
    return bitmaps_[static_cast<size_t>(index)].get();
}
SIZE Size(Id id) {
    const int index = static_cast<int>(id);
    if (index < 0 || index >= kCount) return {0, 0};
    return {UiAssetsData::kEntries[index].width, UiAssetsData::kEntries[index].height};
}
int WidthAt(Id id, int height) {
    const SIZE size = Size(id);
    return size.cy > 0 ? MulDiv(height, size.cx, size.cy) : height;
}
void Draw(Gdiplus::Graphics& graphics, Id id, const Gdiplus::RectF& rect, float opacity, bool mirror) {
    Gdiplus::Bitmap* bitmap = Bitmap(id);
    if (!bitmap || rect.Width <= 0 || rect.Height <= 0 || opacity <= 0) return;
    Quality(graphics);
    Gdiplus::ImageAttributes attributes;
    const float w = static_cast<float>(bitmap->GetWidth()), h = static_cast<float>(bitmap->GetHeight());
    Gdiplus::RectF target = rect;
    if (mirror) { target.X += target.Width; target.Width = -target.Width; }
    const Gdiplus::PointF corners[3] = {{target.X, target.Y}, {target.X + target.Width, target.Y}, {target.X, target.Y + target.Height}};
    graphics.DrawImage(bitmap, corners, 3, 0.f, 0.f, w, h, Gdiplus::UnitPixel, Fade(attributes, opacity));
}
void DrawBar(Gdiplus::Graphics& graphics, Id id, const Gdiplus::RectF& rect, float opacity) {
    Gdiplus::Bitmap* bitmap = Bitmap(id);
    if (!bitmap || rect.Width <= 0 || rect.Height <= 0 || opacity <= 0) return;
    Quality(graphics);
    // Flip-tiling keeps the slice seams from sampling transparent texels.
    Gdiplus::ImageAttributes attributes;
    Fade(attributes, opacity);
    attributes.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
    Gdiplus::ImageAttributes* fade = &attributes;
    const float w = static_cast<float>(bitmap->GetWidth()), h = static_cast<float>(bitmap->GetHeight());
    const float cap_src = (std::min)(w * .2f, h * 1.6f);
    const float cap = (std::min)(rect.Width * .5f, cap_src * rect.Height / h);
    const float middle_src = w - 2 * cap_src;
    graphics.DrawImage(bitmap, Gdiplus::RectF(rect.X, rect.Y, cap, rect.Height), 0, 0, cap_src, h, Gdiplus::UnitPixel, fade);
    if (rect.Width > 2 * cap)
        graphics.DrawImage(bitmap, Gdiplus::RectF(rect.X + cap, rect.Y, rect.Width - 2 * cap, rect.Height), cap_src, 0, middle_src, h, Gdiplus::UnitPixel, fade);
    graphics.DrawImage(bitmap, Gdiplus::RectF(rect.X + rect.Width - cap, rect.Y, cap, rect.Height), w - cap_src, 0, cap_src, h, Gdiplus::UnitPixel, fade);
}
void DrawAvatar(Gdiplus::Graphics& graphics, const Gdiplus::RectF& rect, COLORREF ring, float ring_width) {
    Gdiplus::Bitmap* bitmap = Bitmap(Id::Avatar);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Quality(graphics);
    if (bitmap) {
        Gdiplus::TextureBrush brush(bitmap);
        brush.SetWrapMode(Gdiplus::WrapModeClamp);
        brush.ScaleTransform(rect.Width / static_cast<float>(bitmap->GetWidth()), rect.Height / static_cast<float>(bitmap->GetHeight()));
        brush.TranslateTransform(rect.X, rect.Y, Gdiplus::MatrixOrderAppend);
        graphics.FillEllipse(&brush, rect);
    }
    if (ring_width > 0) {
        Gdiplus::Pen pen(Gdiplus::Color(255, GetRValue(ring), GetGValue(ring), GetBValue(ring)), ring_width);
        const float inset = ring_width / 2.f;
        graphics.DrawEllipse(&pen, rect.X + inset, rect.Y + inset, rect.Width - ring_width, rect.Height - ring_width);
    }
}
const wchar_t* TitleFace() {
    if (!font_tried_) {
        font_tried_ = true;
        DWORD installed = 0;
        font_ = AddFontMemResourceEx(const_cast<unsigned char*>(UiAssetsData::kTitleFont), sizeof(UiAssetsData::kTitleFont), nullptr, &installed);
        if (!installed && font_) { RemoveFontMemResourceEx(font_); font_ = nullptr; }
    }
    return font_ ? L"AaGuDianKeBenSong" : L"";
}
bool Covers(HDC dc, const wchar_t* text, int length) {
    if (!dc || !text) return false;
    if (length < 0) length = static_cast<int>(wcslen(text));
    if (!length) return true;
    std::vector<WORD> indices(static_cast<size_t>(length));
    if (GetGlyphIndicesW(dc, text, length, indices.data(), GGI_MARK_NONEXISTING_GLYPHS) == GDI_ERROR) return false;
    for (int i = 0; i < length; ++i) {
        if (text[i] == L'\r' || text[i] == L'\n' || text[i] == L'\t') continue;
        if (indices[static_cast<size_t>(i)] == 0xffff) return false;
    }
    return true;
}
void Shutdown() {
    for (auto& bitmap : bitmaps_) bitmap.reset();
    tried_.fill(false);
    if (font_) RemoveFontMemResourceEx(font_);
    font_ = nullptr; font_tried_ = false;
}
}
