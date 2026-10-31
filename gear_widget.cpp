#include "gear_widget.h"
#include "air_skills.h"
#include "debug_hub.h"
#include "grab_extras.h"
#include "motian_grab.h"
#include "settings_store.h"
#include "ui_assets.h"
#include "ui_host.h"
#include "ui_i18n.h"
#include "ui_motion.h"
#include "ui_surface.h"
#include "ui_theme.h"
#include <windows.h>
#include <windowsx.h>
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <string>

namespace GearWidget {
namespace {
constexpr wchar_t kClass[] = L"PBZGearWidget_v1";
// Layout in DIPs; everything is multiplied by scale_.
constexpr double kWidth = 360, kBubbleZone = 172, kPanel = 154, kHeight = kBubbleZone + kPanel, kMargin = 28;
constexpr double kRailLeft = 30, kRailRight = 330, kRailY = 236, kRailH = 24, kThumb = 34, kBubble = 128;
constexpr int kFaceFrames = 61, kFaceCell = 112, kFaceColumns = 8;
// Compact pill in the same bottom-right corner; the full panel opens only on a click.
constexpr double kPillW = 228, kPillH = 48, kPillX = kWidth - kPillW, kPillY = kHeight - kPillH;
struct Tier { const wchar_t* name; int preset; };
// The game's own difficulty titles (UI_Setting_13/05/06/11), lowest to highest.
const Tier kTiers[] = {{L"见习者", 0}, {L"入阵者", 4}, {L"破局者", 7}, {L"逆天者", 10}};
// The damage multiplier applies to grabs only, so its chip says so.
const wchar_t* const kChips[] = {L"投技", L"投技伤害", L"JC", L"奖励", L"易触发"};

Logger logger_ = nullptr;
HWND window_ = nullptr;
UiSurface::Surface surface_, mask_;
double scale_ = 1;
int value_ = 0, applied_ = -1;
double displayed_ = 0;
bool hover_pill_ = false;
bool dragging_ = false, scrub_ = false, scrub_moved_ = false, hover_ = false, menu_open_ = false, dirty_ = true, reduced_ = false, expanded_ = false;
int menu_hot_ = -1;
ULONGLONG flash_until_ = 0, bubble_until_ = 0, commit_at_ = 0, last_tick_ = 0, last_inside_ = 0;
UiMotion::Tween opacity_, bubble_, menu_, grow_, expand_;
POINT position_{-1, -1};

void Log(const char* text) { if (logger_) logger_(text); }
int TierOf(int value) { return value <= 2 ? 0 : value <= 5 ? 1 : value <= 8 ? 2 : 3; }
double S(double value) { return value * scale_; }
// Press on the compact pill and drag sideways: one gear per kScrubStep DIP.
constexpr double kScrubStep = 24, kScrubSlop = 5;
int scrub_x_ = 0, scrub_value_ = 0;
bool Held() { return dragging_ || scrub_; }
// Top gear burns: 0 below gear 9, fading in as the dial eases from 9 to 10.
double Fire() { return std::clamp(displayed_ - 9.0, 0.0, 1.0); }
constexpr COLORREF kBlood = RGB(0xc8, 0x2e, 0x1e), kEmber = RGB(0xf0, 0x6a, 0x2c);
ULONGLONG last_fire_frame_ = 0;
double ThumbX(double value) { return kRailLeft + (kRailRight - kRailLeft) * std::clamp(value, 0.0, 10.0) / 10.0; }

// ---- gear -> features ------------------------------------------------------------
struct Plan { bool jc = false, grab = false, rewards1 = false, rewards2 = false, rewards3 = false, invulnerable = false; float radius = 250, multiplier = 1; };
Plan PlanFor(int g) {
    Plan p;
    p.jc = g >= 1; p.radius = g >= 1 ? 150.f + 50.f * static_cast<float>(g - 1) : 250.f;
    p.grab = g >= 2;
    p.rewards1 = g >= 3; p.rewards2 = g >= 5; p.rewards3 = g >= 7;
    p.multiplier = g >= 4 ? 1.f + .5f * static_cast<float>(g - 3) : 1.f;
    p.invulnerable = g >= 9;
    return p;
}
void Apply(int g) {
    const Plan plan = PlanFor(g);
    AirSkills::Options air = AirSkills::GetSnapshot().options;
    air.stomp_jump = plan.jc; air.enemy_radius = plan.radius;
    air.reward_shachi = plan.rewards1;
    air.reward_pure_shachi = air.reward_spirit_shadow = plan.rewards2;
    air.reward_skills = air.reward_weapon_switch = air.reward_guard_dodge = plan.rewards3;
    const bool air_ok = AirSkills::RequestOptions(air);
    MotianGrab::Options grab = MotianGrab::GetSnapshot().options;
    grab.enabled = plan.grab;
    const bool grab_ok = MotianGrab::RequestOptions(grab);
    GrabExtras::Options extras = GrabExtras::GetOptions();
    extras.damage = plan.multiplier > 1.f ? GrabExtras::Damage::Multiplier : GrabExtras::Damage::Native;
    if (plan.multiplier > 1.f) extras.multiplier = plan.multiplier;
    extras.ignore_invulnerable = plan.invulnerable;
    GrabExtras::SetOptions(extras);
    SettingsStore::PutInt(L"Gear", L"Level", g);
    applied_ = g;
    char line[200]{};
    sprintf_s(line, "GEAR level=%d jc=%d radius=%.0f grab=%d rewards=%d/%d/%d damage=x%.1f ignore_invulnerable=%d accepted air=%d grab=%d",
        g, plan.jc ? 1 : 0, plan.radius, plan.grab ? 1 : 0, plan.rewards1 ? 1 : 0, plan.rewards2 ? 1 : 0, plan.rewards3 ? 1 : 0,
        plan.multiplier, plan.invulnerable ? 1 : 0, air_ok ? 1 : 0, grab_ok ? 1 : 0);
    Log(line);
}
// What is actually on now (the detail pages can change it after a gear is set).
struct Live { bool chips[5]{}; std::wstring detail; };
Live Current() {
    Live live;
    const auto air = AirSkills::GetSnapshot().options;
    const auto grab = MotianGrab::GetSnapshot().options;
    const auto extras = GrabExtras::GetOptions();
    live.chips[0] = grab.enabled;
    live.chips[1] = grab.enabled && extras.damage != GrabExtras::Damage::Native;
    live.chips[2] = air.stomp_jump;
    live.chips[3] = air.stomp_jump && (air.reward_shachi || air.reward_pure_shachi || air.reward_spirit_shadow || air.reward_skills);
    live.chips[4] = air.stomp_jump && air.enemy_radius > 250.f;
    wchar_t text[160]{};
    if (live.chips[1]) {
        if (extras.damage == GrabExtras::Damage::Multiplier) swprintf_s(text, L"%s ×%.1f", UiI18n::Translate(L"投技伤害").c_str(), static_cast<double>(extras.multiplier));
        else swprintf_s(text, L"%s %.0f%%", UiI18n::Translate(L"投技伤害").c_str(), static_cast<double>(extras.percent));
        live.detail = text;
    }
    if (air.stomp_jump) {
        swprintf_s(text, L"%s %.0f cm", UiI18n::Translate(L"JC 触发").c_str(), static_cast<double>(air.enemy_radius));
        if (!live.detail.empty()) live.detail += L"  ·  ";
        live.detail += text;
    }
    if (extras.ignore_invulnerable && grab.enabled) { if (!live.detail.empty()) live.detail += L"  ·  "; live.detail += UiI18n::Translate(L"无视无敌"); }
    if (live.detail.empty()) live.detail = UiI18n::Translate(L"全部辅助关闭，保持原生体验");
    return live;
}
// What the gear under the slider will turn on, shown live while it is adjusted
// (the modules only change when the gear is committed).
Live Preview(int g) {
    const Plan plan = PlanFor(g);
    Live live;
    live.chips[0] = plan.grab; live.chips[1] = plan.grab && plan.multiplier > 1.f; live.chips[2] = plan.jc;
    live.chips[3] = plan.jc && plan.rewards1; live.chips[4] = plan.jc && plan.radius > 250.f;
    wchar_t text[160]{};
    if (live.chips[1]) { swprintf_s(text, L"%s ×%.1f", UiI18n::Translate(L"投技伤害").c_str(), static_cast<double>(plan.multiplier)); live.detail = text; }
    if (plan.jc) {
        swprintf_s(text, L"%s %.0f cm", UiI18n::Translate(L"JC 触发").c_str(), static_cast<double>(plan.radius));
        if (!live.detail.empty()) live.detail += L"  ·  ";
        live.detail += text;
    }
    if (plan.invulnerable && plan.grab) { if (!live.detail.empty()) live.detail += L"  ·  "; live.detail += UiI18n::Translate(L"无视无敌"); }
    if (live.detail.empty()) live.detail = UiI18n::Translate(L"全部辅助关闭，保持原生体验");
    return live;
}

// ---- drawing ----------------------------------------------------------------------
Gdiplus::Color Argb(COLORREF c, double alpha = 1) {
    return Gdiplus::Color(static_cast<BYTE>(std::lround(std::clamp(alpha, 0.0, 1.0) * 255)), GetRValue(c), GetGValue(c), GetBValue(c));
}
void Rounded(Gdiplus::GraphicsPath& path, double x, double y, double w, double h, double r) {
    const float d = static_cast<float>((std::min)(r * 2, (std::min)(w, h)));
    const float X = static_cast<float>(x), Y = static_cast<float>(y), W = static_cast<float>(w), H = static_cast<float>(h);
    path.AddArc(X, Y, d, d, 180, 90); path.AddArc(X + W - d, Y, d, d, 270, 90);
    path.AddArc(X + W - d, Y + H - d, d, d, 0, 90); path.AddArc(X, Y + H - d, d, d, 90, 90); path.CloseFigure();
}
// GDI text through a white-on-black mask, blended as premultiplied colour.
void Text(const std::wstring& text, HFONT font, RECT rect, COLORREF color, UINT format, double alpha = 1) {
    if (text.empty() || alpha <= 0) return;
    rect.left = (std::max)(0L, rect.left); rect.top = (std::max)(0L, rect.top);
    rect.right = (std::min)(static_cast<LONG>(mask_.width), rect.right); rect.bottom = (std::min)(static_cast<LONG>(mask_.height), rect.bottom);
    if (rect.right <= rect.left || rect.bottom <= rect.top) return;
    for (int y = rect.top; y < rect.bottom; ++y) std::fill_n(mask_.pixels + static_cast<size_t>(y) * mask_.width + rect.left, rect.right - rect.left, 0u);
    const HGDIOBJ old = SelectObject(mask_.dc, font);
    SetBkMode(mask_.dc, TRANSPARENT); SetTextColor(mask_.dc, RGB(255, 255, 255));
    RECT area = rect; DrawTextW(mask_.dc, text.c_str(), static_cast<int>(text.size()), &area, format | DT_NOPREFIX);
    SelectObject(mask_.dc, old); GdiFlush();
    const double r = GetRValue(color), g = GetGValue(color), b = GetBValue(color);
    for (int y = rect.top; y < rect.bottom; ++y) for (int x = rect.left; x < rect.right; ++x) {
        const size_t index = static_cast<size_t>(y) * mask_.width + x; const uint32_t m = mask_.pixels[index];
        const double cover = alpha * static_cast<double>((std::max)({m & 255, (m >> 8) & 255, (m >> 16) & 255})) / 255.0;
        if (cover <= 0) continue;
        uint32_t& dst = surface_.pixels[index];
        const double inv = 1 - cover;
        const double da = (dst >> 24) * inv + 255 * cover, dr = ((dst >> 16) & 255) * inv + r * cover;
        const double dg = ((dst >> 8) & 255) * inv + g * cover, db = (dst & 255) * inv + b * cover;
        dst = (static_cast<uint32_t>(std::lround(da)) << 24) | (static_cast<uint32_t>(std::lround(dr)) << 16) |
              (static_cast<uint32_t>(std::lround(dg)) << 8) | static_cast<uint32_t>(std::lround(db));
    }
}
RECT R(double x, double y, double w, double h) {
    return {static_cast<LONG>(std::lround(S(x))), static_cast<LONG>(std::lround(S(y))), static_cast<LONG>(std::lround(S(x + w))), static_cast<LONG>(std::lround(S(y + h)))};
}
void Face(Gdiplus::Graphics& graphics, double value, const Gdiplus::RectF& circle, double opacity) {
    Gdiplus::Bitmap* sheet = UiAssets::Bitmap(UiAssets::Id::GrinSheet);
    if (!sheet) return;
    const double f = std::clamp(value / 10.0, 0.0, 1.0) * (kFaceFrames - 1);
    const int i = static_cast<int>(f), j = (std::min)(i + 1, kFaceFrames - 1); const double mix = f - i;
    auto paint = [&](int frame, double alpha) {
        const Gdiplus::RectF cell(static_cast<float>((frame % kFaceColumns) * kFaceCell), static_cast<float>((frame / kFaceColumns) * kFaceCell),
            static_cast<float>(kFaceCell), static_cast<float>(kFaceCell));
        Gdiplus::ImageAttributes attributes; Gdiplus::ColorMatrix matrix{};
        for (int k = 0; k < 5; ++k) matrix.m[k][k] = 1;
        matrix.m[3][3] = static_cast<float>(alpha); attributes.SetColorMatrix(&matrix);
        Gdiplus::TextureBrush brush(sheet, cell, &attributes);
        brush.ScaleTransform(circle.Width / kFaceCell, circle.Height / kFaceCell);
        brush.TranslateTransform(circle.X, circle.Y, Gdiplus::MatrixOrderAppend);
        graphics.FillEllipse(&brush, circle);
    };
    paint(i, opacity);
    if (mix > .001 && j != i) paint(j, mix * opacity);
}
// Premultiplied fade of everything drawn so far (the panel layer).
void FadeAll(double alpha) {
    if (alpha >= .999) return;
    const size_t count = static_cast<size_t>(surface_.width) * surface_.height;
    const uint32_t k = static_cast<uint32_t>(std::lround(std::clamp(alpha, 0.0, 1.0) * 256));
    for (size_t i = 0; i < count; ++i) {
        const uint32_t p = surface_.pixels[i]; if (!p) continue;
        surface_.pixels[i] = (((p >> 24) * k >> 8) << 24) | ((((p >> 16) & 255) * k >> 8) << 16) | ((((p >> 8) & 255) * k >> 8) << 8) | ((p & 255) * k >> 8);
    }
}
void RenderPanel(ULONGLONG now, const Live& live) {
    const int w = surface_.width, h = surface_.height;
    const COLORREF ink = RGB(0x12, 0x11, 0x10), text = UiTheme::Text(), muted = UiTheme::MutedText(), gold = UiTheme::Accent();
    const double menu = menu_.At(now), grow = grow_.At(now);
    const int tier = TierOf(static_cast<int>(std::lround(displayed_)));
    {
        Gdiplus::Bitmap canvas(w, h, w * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(surface_.pixels));
        Gdiplus::Graphics g(&canvas);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias); g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        // Panel: ink plate with a pale-gold hairline, like the game's confirmation boxes.
        Gdiplus::GraphicsPath panel; Rounded(panel, S(1), S(kBubbleZone), S(kWidth - 2), S(kPanel - 2), S(16));
        Gdiplus::SolidBrush plate(Argb(ink, .97)); g.FillPath(&plate, &panel);
        Gdiplus::Pen rim(Argb(gold, .55), static_cast<float>((std::max)(1.0, S(1)))); g.DrawPath(&rim, &panel);
        // Rail with the parchment brush stroke as the fill.
        const double fill_x = ThumbX(displayed_);
        Gdiplus::GraphicsPath rail; Rounded(rail, S(kRailLeft - kRailH / 2), S(kRailY), S(kRailRight - kRailLeft + kRailH), S(kRailH), S(kRailH / 2));
        Gdiplus::SolidBrush groove(Argb(RGB(0x2a, 0x27, 0x23), .95)); g.FillPath(&groove, &rail);
        if (fill_x > kRailLeft - kRailH / 2 + 2) {
            g.SetClip(&rail);
            UiAssets::DrawBar(g, UiAssets::Id::SelectedBar, Gdiplus::RectF(static_cast<float>(S(kRailLeft - kRailH / 2 - 6)), static_cast<float>(S(kRailY - 3)),
                static_cast<float>(S(fill_x - kRailLeft + kRailH / 2 + 8)), static_cast<float>(S(kRailH + 6))), .92f);
            g.ResetClip();
        }
        const double fire = Fire();
        if (fire > .01) {
            // The filled rail turns blood red, with an ember glow at the leading edge.
            g.SetClip(&rail);
            const float left = static_cast<float>(S(kRailLeft - kRailH / 2)), right = static_cast<float>(S(fill_x + kRailH / 2));
            Gdiplus::LinearGradientBrush red(Gdiplus::PointF(left - 1, 0), Gdiplus::PointF(right + 1, 0), Argb(RGB(0x5a, 0x0e, 0x0a), .92 * fire), Argb(kBlood, .95 * fire));
            g.FillRectangle(&red, left, static_cast<float>(S(kRailY)), right - left, static_cast<float>(S(kRailH)));
            const double pulse = reduced_ ? .5 : .5 + .5 * std::sin(static_cast<double>(now % 100000) / 180.0);
            Gdiplus::LinearGradientBrush glow(Gdiplus::PointF(0, static_cast<float>(S(kRailY))), Gdiplus::PointF(0, static_cast<float>(S(kRailY + kRailH))),
                Argb(kEmber, (.25 + .2 * pulse) * fire), Argb(kEmber, 0));
            g.FillRectangle(&glow, left, static_cast<float>(S(kRailY)), right - left, static_cast<float>(S(kRailH)));
            g.ResetClip();
            // The game's painted world-map fire flipbook (6x3 cells), one flame
            // every 22 DIP along the filled rail, each on its own frame phase.
            if (Gdiplus::Bitmap* sheet = UiAssets::Bitmap(UiAssets::Id::FireSheet)) {
                const float cell_w = static_cast<float>(sheet->GetWidth()) / 6.f, cell_h = static_cast<float>(sheet->GetHeight()) / 3.f;
                Gdiplus::ColorMatrix matrix{}; for (int i = 0; i < 5; ++i) matrix.m[i][i] = 1.f;
                matrix.m[3][3] = static_cast<float>(.95 * fire);
                Gdiplus::ImageAttributes fade; fade.SetColorMatrix(&matrix);
                g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBilinear);
                const int count = (std::max)(1, static_cast<int>((fill_x - kRailLeft) / 22.0) + 1);
                for (int i = 0; i < count; ++i) {
                    const double x = kRailLeft + i * 22.0 + (i % 2 ? 4.0 : -3.0);
                    const int frame = reduced_ ? (i * 7) % 18 : static_cast<int>((now / 66 + static_cast<ULONGLONG>(i) * 7) % 18);
                    const double size = 52.0 * (reduced_ ? 1.0 : .84 + .16 * std::sin(static_cast<double>(now % 100000) / 240.0 + i * 1.7));
                    const Gdiplus::RectF target(static_cast<float>(S(x - 26)), static_cast<float>(S(kRailY + kRailH * .8 - size)),
                        static_cast<float>(S(52)), static_cast<float>(S(size)));
                    g.DrawImage(sheet, target, static_cast<float>(frame % 6) * cell_w, static_cast<float>(frame / 6) * cell_h, cell_w, cell_h, Gdiplus::UnitPixel, &fade);
                }
            }
        }
        for (int k = 0; k <= 10; ++k) {
            const double x = ThumbX(k); const bool on = k <= displayed_ + .01;
            Gdiplus::SolidBrush dot(Argb(on ? RGB(0x2a, 0x22, 0x18) : RGB(0x6c, 0x66, 0x5d), on ? .55 : .8));
            const float d = static_cast<float>(S(k % 5 == 0 ? 4 : 3));
            g.FillEllipse(&dot, static_cast<float>(S(x)) - d / 2, static_cast<float>(S(kRailY + kRailH / 2)) - d / 2, d, d);
        }
        // Thumb: parchment disc with a gold ring; grows while held or hovered.
        const double thumb = kThumb * (1 + .08 * grow);
        const Gdiplus::RectF knob(static_cast<float>(S(fill_x - thumb / 2)), static_cast<float>(S(kRailY + kRailH / 2 - thumb / 2)), static_cast<float>(S(thumb)), static_cast<float>(S(thumb)));
        if (grow > .01) { Gdiplus::SolidBrush halo(Argb(gold, .18 * grow)); g.FillEllipse(&halo, knob.X - static_cast<float>(S(5)), knob.Y - static_cast<float>(S(5)), knob.Width + static_cast<float>(S(10)), knob.Height + static_cast<float>(S(10))); }
        Gdiplus::SolidBrush disc(Argb(RGB(0xec, 0xe7, 0xdd))); g.FillEllipse(&disc, knob);
        Gdiplus::Pen ring(Argb(UiTheme::Mix(gold, kBlood, static_cast<int>(std::lround(100 * Fire())))), static_cast<float>(S(2.5))); g.DrawEllipse(&ring, knob.X + static_cast<float>(S(1.25)), knob.Y + static_cast<float>(S(1.25)), knob.Width - static_cast<float>(S(2.5)), knob.Height - static_cast<float>(S(2.5)));
        // Chevron just after the tier name (opens the presets); long English titles push it right.
        double title_w = 120;
        {
            const std::wstring name = UiI18n::Translate(kTiers[tier].name);
            const HGDIOBJ old_font = SelectObject(mask_.dc, UiTheme::GetFontFor(UiTheme::Font::Title, static_cast<UINT>(std::lround(96 * scale_)), name.c_str()));
            SIZE size{}; if (GetTextExtentPoint32W(mask_.dc, name.c_str(), static_cast<int>(name.size()), &size)) title_w = size.cx / scale_;
            SelectObject(mask_.dc, old_font);
        }
        const double chevron_x = (std::min)(kWidth - 16, kWidth / 2 + title_w / 2 + 14), chevron_y = kBubbleZone + 26;
        Gdiplus::Pen arrow(Argb(muted), static_cast<float>(S(1.6)));
        arrow.SetStartCap(Gdiplus::LineCapRound); arrow.SetEndCap(Gdiplus::LineCapRound);
        const Gdiplus::PointF tip(static_cast<float>(S(chevron_x + 3 - 3 * menu)), static_cast<float>(S(chevron_y + 3 * menu)));
        g.DrawLine(&arrow, Gdiplus::PointF(static_cast<float>(S(chevron_x - 2)), static_cast<float>(S(chevron_y - 5 + 2 * menu))), tip);
        g.DrawLine(&arrow, tip, Gdiplus::PointF(static_cast<float>(S(chevron_x - 2 + 7 * menu)), static_cast<float>(S(chevron_y + 5 - 2 * menu))));
        // Feature chips: gold when actually on.
        const double chip_w = (kWidth - 40) / 5.0;
        for (int k = 0; k < 5; ++k) {
            const double x = 20 + k * chip_w + 4, y = kRailY + kRailH + 16;
            Gdiplus::GraphicsPath chip; Rounded(chip, S(x), S(y), S(chip_w - 8), S(22), S(11));
            Gdiplus::SolidBrush fill(Argb(live.chips[k] ? gold : RGB(0x3a, 0x36, 0x31), live.chips[k] ? .22 : .55)); g.FillPath(&fill, &chip);
            Gdiplus::Pen edge(Argb(live.chips[k] ? gold : RGB(0x4a, 0x45, 0x3e), live.chips[k] ? .8 : .7), static_cast<float>((std::max)(1.0, S(1)))); g.DrawPath(&edge, &chip);
        }
        // Preset menu (replaces the bubble zone while open).
        if (menu > .01) {
            const double top = kBubbleZone - 8 - 4 * 34 - 14 + (1 - menu) * 10;
            Gdiplus::GraphicsPath box; Rounded(box, S(60), S(top), S(240), S(4 * 34 + 14), S(12));
            Gdiplus::SolidBrush back(Argb(ink, .96 * menu)); g.FillPath(&back, &box);
            Gdiplus::Pen line(Argb(gold, .45 * menu), static_cast<float>((std::max)(1.0, S(1)))); g.DrawPath(&line, &box);
            for (int k = 0; k < 4; ++k) {
                const bool selected = TierOf(value_) == k;
                if (selected || menu_hot_ == k)
                    UiAssets::DrawBar(g, UiAssets::Id::SelectedBar, Gdiplus::RectF(static_cast<float>(S(68)), static_cast<float>(S(top + 7 + k * 34)), static_cast<float>(S(224)), static_cast<float>(S(32))),
                        static_cast<float>(menu * (selected ? .9 : .25)));
            }
        }
    }
    const UINT dpi = static_cast<UINT>(std::lround(96 * scale_));
    const std::wstring tier_name = UiI18n::Translate(kTiers[tier].name);
    HFONT title = UiTheme::GetFontFor(UiTheme::Font::Title, dpi, tier_name.c_str());
    Text(tier_name, title, R(0, kBubbleZone + 8, kWidth, 38), tier == 3 ? RGB(0xe0, 0x8a, 0x6a) : tier == 2 ? gold : text, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    HFONT small_font = UiTheme::GetFont(UiTheme::Font::Small, dpi);
    const double chip_w = (kWidth - 40) / 5.0;
    for (int k = 0; k < 5; ++k)
        Text(UiI18n::Translate(kChips[k]), small_font, R(20 + k * chip_w + 4, kRailY + kRailH + 16, chip_w - 8, 22), live.chips[k] ? RGB(0xf2, 0xe6, 0xc8) : muted, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    Text(live.detail, small_font, R(16, kRailY + kRailH + 44, kWidth - 32, 22), muted, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);
    if (menu > .01) {
        const double top = kBubbleZone - 8 - 4 * 34 - 14 + (1 - menu) * 10;
        Text(UiI18n::Translate(L"挡位 · 0–10"), small_font, R(76, top - 22, 208, 20), muted, DT_SINGLELINE | DT_CENTER | DT_VCENTER, menu);
        for (int k = 0; k < 4; ++k) {
            const bool selected = TierOf(value_) == k;
            const std::wstring name = UiI18n::Translate(kTiers[k].name);
            Text(name, UiTheme::GetFontFor(UiTheme::Font::Heading, dpi, name.c_str()), R(82, top + 7 + k * 34, 120, 32), selected ? RGB(0x14, 0x12, 0x10) : text, DT_SINGLELINE | DT_LEFT | DT_VCENTER, menu);
            wchar_t level[32]{}; swprintf_s(level, UiI18n::Translate(L"%d 档").c_str(), kTiers[k].preset);
            Text(level, small_font, R(200, top + 7 + k * 34, 86, 32), selected ? RGB(0x3a, 0x30, 0x22) : muted, DT_SINGLELINE | DT_RIGHT | DT_VCENTER, menu);
        }
    }
}
// Compact pill: gear ring with the number, tier name and an expand chevron.
void RenderPill(double alpha) {
    const int w = surface_.width, h = surface_.height;
    const COLORREF ink = RGB(0x12, 0x11, 0x10), text = UiTheme::Text(), muted = UiTheme::MutedText(), gold = UiTheme::Accent();
    const int tier = TierOf(static_cast<int>(std::lround(displayed_)));
    const double ring_d = 34, ring_x = kPillX + 8, ring_y = kPillY + (kPillH - ring_d) / 2;
    {
        Gdiplus::Bitmap canvas(w, h, w * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(surface_.pixels));
        Gdiplus::Graphics g(&canvas);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias); g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        Gdiplus::GraphicsPath pill; Rounded(pill, S(kPillX + 1), S(kPillY + 1), S(kPillW - 2), S(kPillH - 2), S(kPillH / 2));
        Gdiplus::SolidBrush plate(Argb(ink, .9 * alpha)); g.FillPath(&plate, &pill);
        Gdiplus::Pen rim(Argb(gold, (hover_pill_ ? .85 : .5) * alpha), static_cast<float>((std::max)(1.0, S(1)))); g.DrawPath(&rim, &pill);
        const Gdiplus::RectF ring(static_cast<float>(S(ring_x + 2)), static_cast<float>(S(ring_y + 2)), static_cast<float>(S(ring_d - 4)), static_cast<float>(S(ring_d - 4)));
        Gdiplus::Pen track(Argb(RGB(0x3a, 0x36, 0x31), alpha), static_cast<float>(S(3))); g.DrawEllipse(&track, ring);
        if (displayed_ > .01) {
            Gdiplus::Pen arc(Argb(UiTheme::Mix(tier == 3 ? RGB(0xe0, 0x8a, 0x6a) : gold, kBlood, static_cast<int>(std::lround(100 * Fire()))), alpha), static_cast<float>(S(3)));
            arc.SetStartCap(Gdiplus::LineCapRound); arc.SetEndCap(Gdiplus::LineCapRound);
            g.DrawArc(&arc, ring, -90.f, static_cast<float>(360.0 * displayed_ / 10.0));
        }
        Gdiplus::Pen arrow(Argb(muted, alpha), static_cast<float>(S(1.6)));
        arrow.SetStartCap(Gdiplus::LineCapRound); arrow.SetEndCap(Gdiplus::LineCapRound);
        const double cx = kPillX + kPillW - 20, cy = kPillY + kPillH / 2;
        g.DrawLine(&arrow, Gdiplus::PointF(static_cast<float>(S(cx - 4)), static_cast<float>(S(cy + 2))), Gdiplus::PointF(static_cast<float>(S(cx)), static_cast<float>(S(cy - 3))));
        g.DrawLine(&arrow, Gdiplus::PointF(static_cast<float>(S(cx)), static_cast<float>(S(cy - 3))), Gdiplus::PointF(static_cast<float>(S(cx + 4)), static_cast<float>(S(cy + 2))));
    }
    const UINT dpi = static_cast<UINT>(std::lround(96 * scale_));
    wchar_t number[8]{}; swprintf_s(number, L"%d", static_cast<int>(std::lround(displayed_)));
    Text(number, UiTheme::GetFont(UiTheme::Font::Strong, dpi), R(ring_x, ring_y, ring_d, ring_d), text, DT_SINGLELINE | DT_CENTER | DT_VCENTER, alpha);
    const std::wstring name = UiI18n::Translate(kTiers[tier].name);
    Text(name, UiTheme::GetFontFor(UiTheme::Font::Heading, dpi, name.c_str()), R(ring_x + ring_d + 10, kPillY, kPillW - ring_d - 50, kPillH),
        tier == 3 ? RGB(0xe0, 0x8a, 0x6a) : tier == 2 ? gold : text, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS, alpha);
}
// The face bubble, above the thumb (panel) or above the pill (compact).
void RenderBubble(ULONGLONG now, double expand) {
    const double bubble = bubble_.At(now);
    if (bubble <= .01 || menu_.At(now) >= .5) return;
    const int w = surface_.width, h = surface_.height;
    const COLORREF gold = UiTheme::Accent();
    const double scale = .82 + .18 * bubble, size = kBubble * scale;
    const double panel_x = std::clamp(ThumbX(displayed_), kBubble / 2 + 8, kWidth - kBubble / 2 - 8), pill_x = kPillX + kPillW / 2;
    const double cx = pill_x + (panel_x - pill_x) * expand;
    const double cy = (kPillY - 16 - kBubble / 2) + ((kBubbleZone - 16 - kBubble / 2) - (kPillY - 16 - kBubble / 2)) * expand + (1 - bubble) * 12;
    const double anchor = ThumbX(displayed_) * expand + (kPillX + 25) * (1 - expand);
    {
        Gdiplus::Bitmap canvas(w, h, w * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(surface_.pixels));
        Gdiplus::Graphics g(&canvas);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias); g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        const Gdiplus::RectF circle(static_cast<float>(S(cx - size / 2)), static_cast<float>(S(cy - size / 2)), static_cast<float>(S(size)), static_cast<float>(S(size)));
        const double px = std::clamp(anchor, cx - 30, cx + 30);
        Gdiplus::PointF pointer[3] = {{static_cast<float>(S(px - 8)), static_cast<float>(S(cy + size / 2 - 3))}, {static_cast<float>(S(px + 8)), static_cast<float>(S(cy + size / 2 - 3))},
            {static_cast<float>(S(px)), static_cast<float>(S(cy + size / 2 + 9))}};
        Gdiplus::SolidBrush tipb(Argb(gold, bubble)); g.FillPolygon(&tipb, pointer, 3);
        Gdiplus::SolidBrush shadow(Argb(RGB(0, 0, 0), .35 * bubble));
        g.FillEllipse(&shadow, circle.X + static_cast<float>(S(2)), circle.Y + static_cast<float>(S(6)), circle.Width, circle.Height);
        Face(g, displayed_, circle, bubble);
        // Bottom shade for the gear label; spans the whole circle so it never tiles.
        Gdiplus::LinearGradientBrush shade(Gdiplus::PointF(0, circle.Y - 1), Gdiplus::PointF(0, circle.Y + circle.Height + 1), Argb(RGB(0, 0, 0), 0), Argb(RGB(6, 5, 4), .7 * bubble));
        const Gdiplus::Color stops[3] = {Argb(RGB(0, 0, 0), 0), Argb(RGB(0, 0, 0), 0), Argb(RGB(6, 5, 4), .7 * bubble)};
        const float positions[3] = {0.f, .55f, 1.f};
        shade.SetInterpolationColors(stops, positions, 3);
        g.FillEllipse(&shade, circle);
        Gdiplus::Pen gilt(Argb(gold, bubble), static_cast<float>(S(3))); g.DrawEllipse(&gilt, circle.X + static_cast<float>(S(1.5)), circle.Y + static_cast<float>(S(1.5)), circle.Width - static_cast<float>(S(3)), circle.Height - static_cast<float>(S(3)));
    }
    wchar_t level[32]{}; swprintf_s(level, UiI18n::Translate(L"%d 档").c_str(), static_cast<int>(std::lround(displayed_)));
    Text(level, UiTheme::GetFont(UiTheme::Font::Small, static_cast<UINT>(std::lround(96 * scale_))), R(cx - size / 2, cy + size / 2 - 30, size, 22), RGB(0xf6, 0xf1, 0xe6), DT_SINGLELINE | DT_CENTER | DT_VCENTER, bubble);
}
void Render(ULONGLONG now) {
    const int w = static_cast<int>(std::lround(S(kWidth))), h = static_cast<int>(std::lround(S(kHeight)));
    if (!surface_.Ensure(w, h) || !mask_.Ensure(w, h)) return;
    surface_.Clear();
    const double expand = expand_.At(now);
    if (expand > .001) { RenderPanel(now, value_ != applied_ || Held() ? Preview(value_) : Current()); FadeAll(expand); }
    if (expand < .999) RenderPill(1 - expand);
    RenderBubble(now, expand);
}

// ---- window -------------------------------------------------------------------------
POINT Dip(LPARAM lparam) { return {static_cast<LONG>(GET_X_LPARAM(lparam) / scale_), static_cast<LONG>(GET_Y_LPARAM(lparam) / scale_)}; }
bool Expanded() { return expanded_ || expand_.At(GetTickCount64()) > .5; }
bool OnPill(POINT p) { return p.x >= kPillX && p.x <= kWidth && p.y >= kPillY && p.y <= kHeight; }
bool OnPanel(POINT p) { return p.y >= kBubbleZone; }
void SetExpanded(bool expanded, ULONGLONG now) {
    if (expanded == expanded_) return;
    expanded_ = expanded; expand_.ease = expanded ? UiMotion::EaseOutCubic : UiMotion::EaseInCubic;
    expand_.Set(expanded ? 1 : 0, expanded ? 240 : 180, now, reduced_); last_inside_ = now; dirty_ = true;
    if (!expanded && menu_open_) { menu_open_ = false; menu_.Set(0, 120, now, reduced_); }
}
bool OnRail(POINT p) { if (!Expanded()) return false; return p.x >= kRailLeft - kThumb / 2 - 4 && p.x <= kRailRight + kThumb / 2 + 4 && p.y >= kRailY - 12 && p.y <= kRailY + kRailH + 12; }
bool OnTitle(POINT p) { if (!Expanded()) return false; return p.y >= kBubbleZone + 6 && p.y <= kBubbleZone + 46 && p.x >= kWidth / 2 - 90 && p.x <= kWidth / 2 + 90; }
int MenuRow(POINT p) {
    if (!menu_open_) return -1;
    const double top = kBubbleZone - 8 - 4 * 34 - 14;
    if (p.x < 60 || p.x > 300 || p.y < top + 7 || p.y >= top + 7 + 4 * 34) return -1;
    return static_cast<int>((p.y - top - 7) / 34);
}
void ShowBubble(ULONGLONG now) {
    bubble_.ease = UiMotion::EaseOutCubic; bubble_.Set(1, 200, now, reduced_); bubble_until_ = 0; dirty_ = true;
}
void HideBubble(ULONGLONG now, ULONGLONG delay) { bubble_until_ = now + delay; }
void SetValue(int value, ULONGLONG now) {
    value = std::clamp(value, 0, 10);
    if (value == value_) return;
    value_ = value; commit_at_ = now + 300; dirty_ = true;
}
void SetFromX(int x_px, ULONGLONG now) {
    const double x = x_px / scale_;
    SetValue(static_cast<int>(std::lround((x - kRailLeft) / (kRailRight - kRailLeft) * 10)), now);
}
void ToggleMenu(ULONGLONG now) {
    menu_open_ = !menu_open_; menu_.ease = UiMotion::EaseOutCubic; menu_.Set(menu_open_ ? 1 : 0, 180, now, reduced_);
    if (menu_open_) bubble_.Set(0, 120, now, reduced_);
    dirty_ = true;
}
LRESULT CALLBACK Procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    const ULONGLONG now = GetTickCount64();
    switch (message) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_NCHITTEST: {
        POINT p{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}; ScreenToClient(hwnd, &p);
        const POINT d{static_cast<LONG>(p.x / scale_), static_cast<LONG>(p.y / scale_)};
        // Only the visible pill / panel take the mouse; the rest stays click-through.
        return (Expanded() ? OnPanel(d) : OnPill(d)) || MenuRow(d) >= 0 || Held() ? HTCLIENT : HTTRANSPARENT;
    }
    case WM_SETCURSOR: {
        POINT p{}; GetCursorPos(&p); ScreenToClient(hwnd, &p);
        const POINT d{static_cast<LONG>(p.x / scale_), static_cast<LONG>(p.y / scale_)};
        SetCursor(LoadCursorW(nullptr, OnRail(d) || OnTitle(d) || MenuRow(d) >= 0 || (!Expanded() && OnPill(d)) ? IDC_HAND : IDC_ARROW)); return TRUE;
    }
    case WM_LBUTTONDOWN: {
        const POINT d = Dip(lparam);
        if (!Expanded()) {
            if (!OnPill(d)) return 0;
            SetExpanded(true, now);
            scrub_ = true; scrub_moved_ = false; scrub_x_ = GET_X_LPARAM(lparam); scrub_value_ = value_; SetCapture(hwnd);
            return 0;
        }
        last_inside_ = now;
        if (const int row = MenuRow(d); row >= 0) { SetValue(kTiers[row].preset, now); ToggleMenu(now); ShowBubble(now); HideBubble(now, 700); return 0; }
        if (OnTitle(d)) { ToggleMenu(now); return 0; }
        if (OnRail(d)) {
            if (menu_open_) ToggleMenu(now);
            dragging_ = true; SetCapture(hwnd); ShowBubble(now); grow_.Set(1, 140, now, reduced_);
            SetFromX(GET_X_LPARAM(lparam), now);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!hover_) { TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0}; TrackMouseEvent(&track); hover_ = true; }
        const POINT d = Dip(lparam);
        if (Expanded() && (OnPanel(d) || MenuRow(d) >= 0)) last_inside_ = now;
        if (!Expanded()) { const bool over = OnPill(d); if (over != hover_pill_) { hover_pill_ = over; dirty_ = true; } }
        if (dragging_) SetFromX(GET_X_LPARAM(lparam), now);
        if (scrub_) {
            const double dx = (GET_X_LPARAM(lparam) - scrub_x_) / scale_;
            if (!scrub_moved_ && std::fabs(dx) >= kScrubSlop) { scrub_moved_ = true; ShowBubble(now); grow_.Set(1, 140, now, reduced_); }
            if (scrub_moved_) { last_inside_ = now; SetValue(scrub_value_ + static_cast<int>(std::lround(dx / kScrubStep)), now); }
        }
        const int row = MenuRow(d); if (row != menu_hot_) { menu_hot_ = row; dirty_ = true; }
        const bool close = OnRail(d) || Held();
        if (close && grow_.to < 1) grow_.Set(1, 140, now, reduced_); else if (!close && grow_.to > 0) grow_.Set(0, 200, now, reduced_);
        return 0;
    }
    case WM_MOUSELEAVE: hover_ = false; hover_pill_ = false; menu_hot_ = -1; if (!Held()) grow_.Set(0, 200, now, reduced_); dirty_ = true; return 0;
    case WM_LBUTTONUP: case WM_CAPTURECHANGED: {
        // Clear the flags before ReleaseCapture: it sends WM_CAPTURECHANGED re-entrantly.
        const bool moved = dragging_ || (scrub_ && scrub_moved_), held = Held();
        dragging_ = scrub_ = scrub_moved_ = false;
        if (held && message == WM_LBUTTONUP && GetCapture() == hwnd) ReleaseCapture();
        if (moved) { last_inside_ = now; HideBubble(now, 350); grow_.Set(0, 200, now, reduced_); }
        return 0;
    }
    case WM_MOUSEWHEEL:
        last_inside_ = now;
        SetValue(value_ + (GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? 1 : -1), now); ShowBubble(now); HideBubble(now, 650); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_NCDESTROY: if (window_ == hwnd) window_ = nullptr; break;
    default: break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
bool Create() {
    if (window_) return true;
    WNDCLASSW cls{}; cls.lpfnWndProc = Procedure; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = kClass; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    window_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE, kClass, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, cls.hInstance, nullptr);
    if (!window_) return false;
    if (!UiHost::Attach(window_)) { DestroyWindow(window_); window_ = nullptr; return false; }
    return true;
}
}

void Initialize(Logger logger) {
    logger_ = logger; reduced_ = UiMotion::ReducedMotion();
    value_ = std::clamp(SettingsStore::GetInt(L"Gear", L"Level", 0), 0, 10);
    applied_ = value_; displayed_ = value_;
    opacity_.Jump(0); bubble_.Jump(0); menu_.Jump(0); grow_.Jump(0); expand_.Jump(0); expanded_ = false;
}
void Nudge(int delta) {
    const ULONGLONG now = GetTickCount64();
    SetValue(value_ + delta, now);
    flash_until_ = now + 1800; ShowBubble(now); HideBubble(now, 1300);
}
int Level() { return value_; }
bool Active() { return window_ && IsWindowVisible(window_); }
void Tick() {
    if (!logger_) return;
    const ULONGLONG now = GetTickCount64();
    const double dt = last_tick_ ? static_cast<double>(now - last_tick_) : 16.0; last_tick_ = now;
    UiHost::Tick();
    const bool want = UiHost::InGame() && (DebugHub::Visible() || now < flash_until_ || Held());
    if (want && !window_ && !Create()) return;
    if (!window_) return;
    if (commit_at_ && now >= commit_at_ && !Held()) { commit_at_ = 0; if (value_ != applied_) Apply(value_); }
    if (want && opacity_.to < 1) { opacity_.ease = UiMotion::EaseOutCubic; opacity_.Set(1, 220, now, reduced_); }
    if (!want && opacity_.to > 0) { opacity_.ease = UiMotion::EaseInCubic; opacity_.Set(0, 180, now, reduced_); if (menu_open_) ToggleMenu(now); }
    if (expanded_) {
        // A resting cursor sends no mouse moves: keep the panel open while the pointer is really over it.
        POINT cursor{};
        if (GetCursorPos(&cursor) && ScreenToClient(window_, &cursor)) {
            const POINT d{static_cast<LONG>(cursor.x / scale_), static_cast<LONG>(cursor.y / scale_)};
            if (d.x >= 0 && d.x <= kWidth && (OnPanel(d) || MenuRow(d) >= 0) && d.y <= kHeight) last_inside_ = now;
        }
        if (!Held() && !menu_open_ && (!want || now - last_inside_ > 1600)) SetExpanded(false, now);
    }
    if (bubble_until_ && now >= bubble_until_ && !Held()) { bubble_until_ = 0; bubble_.ease = UiMotion::EaseInCubic; bubble_.Set(0, 160, now, reduced_); }
    // The dial eases toward the chosen gear (exp. smoothing like the reference).
    const double target = value_;
    if (std::fabs(target - displayed_) > .002) { displayed_ = reduced_ ? target : displayed_ + (target - displayed_) * (1 - std::exp(-dt / 110.0)); dirty_ = true; }
    else if (displayed_ != target) { displayed_ = target; dirty_ = true; }
    // One extra frame after the last animation step, so the final state is drawn.
    static bool was_animating = false;
    if (!reduced_ && Fire() > .01 && expand_.At(now) > .001 && now - last_fire_frame_ >= 33) { last_fire_frame_ = now; dirty_ = true; }
    const bool running = opacity_.Running(now) || bubble_.Running(now) || menu_.Running(now) || grow_.Running(now) || expand_.Running(now);
    const bool animating = running || was_animating;
    was_animating = running;
    const double alpha = opacity_.At(now);
    if (alpha <= .001 && !want) { if (IsWindowVisible(window_)) ShowWindow(window_, SW_HIDE); return; }
    // Bottom-right of the game client, above the game HUD margin.
    const HWND game = UiHost::Game(); const UINT dpi = game ? GetDpiForWindow(game) : 96;
    const double scale = std::clamp(static_cast<double>(dpi ? dpi : 96) / 96.0, .75, 2.5);
    if (scale != scale_) { scale_ = scale; dirty_ = true; }
    const SIZE client = UiHost::ClientSize(); const POINT origin = UiHost::ClientOrigin();
    const POINT at{origin.x + client.cx - static_cast<LONG>(std::lround(S(kWidth + kMargin))), origin.y + client.cy - static_cast<LONG>(std::lround(S(kHeight + kMargin)))};
    static int last_alpha = -1;
    const int byte = static_cast<int>(std::lround(alpha * 255));
    if (!dirty_ && !animating && byte == last_alpha && at.x == position_.x && at.y == position_.y && IsWindowVisible(window_)) return;
    Render(now); dirty_ = false; last_alpha = byte; position_ = at;
    surface_.Present(window_, at, static_cast<BYTE>(byte));
    if (!IsWindowVisible(window_)) { ShowWindow(window_, SW_SHOWNA); SetWindowPos(window_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); }
}
void Shutdown() {
    if (window_) { UiHost::Detach(window_); DestroyWindow(window_); }
    window_ = nullptr; surface_.Reset(); mask_.Reset(); logger_ = nullptr;
}
}
