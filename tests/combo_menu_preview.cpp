// Offline preview of the 招式派生 page (combo_menu.cpp): real generated skill / dodge tables, stubbed game state,
// a saved canvas with intervals, triggers, a 按键分支 and the 加速 / 霸体 / 武器属性 modifiers, rendered to a PNG, then
// scripted mouse and keyboard checks.
//   combo_menu_preview.exe <out.png> [skin index] [width] [en]
#include "combo_menu.h"
#include "ui_cursor.h"
#include "combo_weapons.h"
#include "npc_combo.h"
#include "npc_retarget.h"
#include "motian_grab.h"
#include "test_dummy.h"
#include "move_preview.h"
#include "speed_curve_window.h"
#include "bone_motion.h"
#include "grab_boss.h"
#include "debug_hub.h"
#include "ui_hotkeys.h"
#include "ui_i18n.h"
#include "ui_theme.h"
#include "../src/generated/npc_skills.generated.h"
#include "../src/generated/npc_dodges.generated.h"
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>

namespace {
int skin_ = 0;
HWND host_ = nullptr, page_ = nullptr;
int min_height_ = 600;
int dummy_requests_ = 0;
bool dummy_standing_ = false;
int preview_from_=0,preview_to_=0;std::vector<int> preview_holds_;
BoneMotion::Config preview_bones_;
std::string preview_shown_;  // 招式预览 stand-in: the package the page asked for (empty: hidden)
}
// 测试木桩 stand-in: a request stands it up / takes it away at once (the game thread does that in the game).
namespace BoneMotionRuntime {
BoneMotion::Config Global(){return {};}
void SetGlobal(const BoneMotion::Config&,bool){}
Snapshot Get(){Snapshot s;s.mesh="fixture";s.bones={"root","pelvis","head","thigh_l","FACIAL_L_LipCorner","jaw","breast_l"};s.parents={-1,0,1,1,2,2,1};s.positions={{{0,0,0}},{{0,0,90}},{{0,0,180}},{{0,-10,50}},{{0,0,177}},{{0,0,170}},{{5,-10,130}}};return s;}
void Debug(bool){}
void Preview(const BoneMotion::Config& c){preview_bones_=c;}
void Supplement(bool,bool){}
bool SupplementEnabled(){return false;}
}
namespace TestDummy {
void Request(bool spawn, double) { ++dummy_requests_; dummy_standing_ = spawn; }
Snapshot GetSnapshot() { Snapshot s; s.present = dummy_standing_; return s; }
}

// 招式预览 stand-in: what was asked is kept; one grey frame comes back.
namespace MovePreview {
void Show(const std::string&, const std::string& ability, int, const std::vector<NpcCombo::CurvePoint>&, int from, int to, const BoneMotion::Config& bones,const std::vector<int>& holds) { preview_holds_=holds;preview_shown_ = ability; preview_from_=from; preview_to_=to; preview_bones_=bones; }
void Hide() { preview_shown_.clear(); }
void Place(const RECT&) {}
bool Latest(Frame& frame, uint64_t newer_than) {
    if (preview_shown_.empty() || newer_than >= 1) return false;
    frame.pixels.assign(static_cast<size_t>(kWidth) * kHeight, 0);
    for (int y = 0; y < kHeight; ++y) for (int x = 0; x < kWidth; ++x) { const uint32_t g = 80 + static_cast<uint32_t>((x + y) % 90); frame.pixels[static_cast<size_t>(y) * kWidth + x] = 0xff000000u | (g << 16) | (g << 8) | g; }
    frame.serial = 1; frame.status.clear(); return true;
}
}
namespace NpcRetarget {
bool MovePreviewSubject(int32_t skin, const std::string& ability, std::string& monster, std::string& package) {
    monster.clear(); package.clear();
    if (skin < 0 || static_cast<size_t>(skin) >= std::size(kNpcSkillSets) || !kNpcSkillSets[skin].monster) return false;
    const NpcSkillSet& set = kNpcSkillSets[skin];
    for (size_t i = 0; i < set.skill_count; ++i) if (NpcCombo::ShortName(set.skills[i]) == ability) { package = set.skills[i]; monster = set.monster; return true; }
    const NpcDodgeSet& dodges = kNpcDodgeSets[skin];  // a dodge pill too, as MovePackage
    for (size_t i = 0; i < dodges.count; ++i) if (NpcCombo::ShortName(dodges.dodges[i].ability) == ability) { package = dodges.dodges[i].ability; monster = set.monster; return true; }
    return false;
}
Snapshot GetSnapshot() {
    Snapshot s{}; s.skin = skin_; s.chain = skin_;
    for (size_t i = 0; i < std::size(kNpcSkillSets); ++i) { std::string id = kNpcSkillSets[i].profile; s.entries.push_back({static_cast<int32_t>(i), id, std::wstring(id.begin(), id.end()), L""}); }
    return s;
}
bool GetSkinMoves(int32_t skin, SkinMoves& moves) {
    moves = {};
    if (skin == -1) { moves.profile = NpcCombo::kHeroProfile; moves.name = L"主角"; moves.skills = {"GA_Hun_LightAttack_01", "GA_Hun_HeavyAttack_01", "GA_Hun_Dodge"}; return true; }
    if (skin < 0 || static_cast<size_t>(skin) >= std::size(kNpcSkillSets)) return false;
    moves.profile = kNpcSkillSets[skin].profile; moves.name = std::wstring(moves.profile.begin(), moves.profile.end());
    std::set<std::string> seen;
    const NpcDodgeSet& dodges = kNpcDodgeSets[skin];
    for (size_t i = 0; i < dodges.count; ++i)
        if (seen.insert(NpcCombo::ShortName(dodges.dodges[i].ability)).second) { moves.dodges.push_back(NpcCombo::ShortName(dodges.dodges[i].ability)); moves.directions.push_back(dodges.dodges[i].dir); }
    const NpcSkillSet& set = kNpcSkillSets[skin];
    for (size_t i = 0; i < set.skill_count; ++i) if (seen.insert(NpcCombo::ShortName(set.skills[i])).second) moves.skills.push_back(NpcCombo::ShortName(set.skills[i]));
    return true;
}
// Stand-in for the grab-to-look match: the first skin of the same boss.
int32_t SkinForGrab(const std::wstring& boss_id, const std::wstring&) {
    for (size_t i = 0; i < std::size(kNpcSkillSets); ++i) if (GrabBoss::Same(std::string_view(kNpcSkillSets[i].profile), boss_id)) return static_cast<int32_t>(i);
    return -1;
}
bool GrabPreviewMonster(const std::wstring& boss_id, const std::wstring& source, std::string& monster) {
    const int32_t row = SkinForGrab(boss_id, source);
    monster = row >= 0 && kNpcSkillSets[row].monster ? kNpcSkillSets[row].monster : "";
    return !monster.empty();
}
// Stand-in for SkillLabel: the name without "GA_<boss>_".
std::wstring MoveLabel(const std::string& ability) {
    std::string name = NpcCombo::ShortName(ability);
    const auto tokens = NpcCombo::Tokens(name);
    std::string out;
    for (size_t i = 2; i < tokens.size(); ++i) out += (out.empty() ? "" : " ") + tokens[i];
    if (out.empty()) out = name;
    return std::wstring(out.begin(), out.end());
}
}
namespace DebugHub {
void Attach(HWND child, HWND parent, int, int) { SetParent(child, parent); ShowWindow(child, SW_SHOW); }
// As the hub: the page is at least its minimum and at least the visible area.
void SetMinimum(HWND page, int, int height) {
    min_height_ = height; RECT r{}; GetClientRect(host_, &r);
    SetWindowPos(page, nullptr, 0, 0, r.right, (std::max)(static_cast<LONG>(height), r.bottom), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}
bool Notify(const wchar_t* text) { std::wprintf(L"notify: %ls\n", text); return true; }
bool SetPreviewHole(const RECT*) { return false; }
void ShowPage(Page) {}
bool PreviewHoleSupported() { return false; }
void Hide() {}
}
namespace UiCursor {
bool hands_off_ = false;  // combo handsoff: the page's test switch (the real one keeps the hub off the mouse)
void SetHandsOff(bool on) { hands_off_ = on; }
bool HandsOff() { return hands_off_; }
}
namespace UiHotkeys {
std::wstring Hint(Action) { return L"L3 + R3"; }
unsigned g_pad = 0;  // the pad buttons held, as the hotkey module would publish them
unsigned g_stick = 0;  // the left stick's way (1 up, 2 down, 4 left, 8 right)
PadLevel CurrentPad() { PadLevel level; level.buttons = g_pad; level.stick = g_stick; level.in_game = true; return level; }
Device ActivePad() { return Device::Xbox; }
// Icon stand-ins (the game's prompt icons are not here): a rounded box per glyph with its name, sized as the module does.
const wchar_t* const kPadNames[16] = {L"Up", L"Dn", L"Lt", L"Rt", L"Menu", L"View", L"L3", L"R3", L"LB", L"RB", L"A", L"B", L"X", L"Y", L"LT", L"RT"};
std::vector<std::wstring> GlyphNames(const Chord& chord) {
    std::vector<std::wstring> out;
    if (chord.device == Device::Keyboard) { wchar_t text[16]{}; if (chord.key >= '0' && chord.key <= 'Z') text[0] = static_cast<wchar_t>(chord.key); else _snwprintf_s(text, _countof(text), _TRUNCATE, L"%02X", chord.key); out.push_back(text); return out; }
    for (int bit = 0; bit < 16; ++bit) if (chord.buttons & (1u << bit)) out.push_back(kPadNames[bit]);
    return out;
}
int TextWidth(HDC dc, int height, const std::wstring& text) {
    HFONT font = CreateFontW(-(std::max)(8, height * 3 / 5), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    const HGDIOBJ old = SelectObject(dc, font); SIZE size{}; GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size); SelectObject(dc, old); DeleteObject(font);
    return (std::max)(height, static_cast<int>(size.cx) + height * 11 / 20);
}
void Box(HDC dc, int x, int y, int width, int height, const std::wstring& text, COLORREF fill) {
    HBRUSH brush = CreateSolidBrush(fill); const HGDIOBJ old_brush = SelectObject(dc, brush), old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, x, y, x + width, y + height, height / 3, height / 3); SelectObject(dc, old_brush); SelectObject(dc, old_pen); DeleteObject(brush);
    HFONT font = CreateFontW(-(std::max)(8, height * 3 / 5), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    const HGDIOBJ old = SelectObject(dc, font); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(16, 16, 16));
    RECT r{x, y, x + width, y + height}; DrawTextW(dc, text.c_str(), -1, &r, DT_SINGLELINE | DT_VCENTER | DT_CENTER); SelectObject(dc, old); DeleteObject(font);
}
Chord Get(Action) { Chord chord; chord.device = Device::Xbox; chord.buttons = (1u << 6) | (1u << 7); return chord; }
int Width(HDC dc, int height, const Chord& chord) {
    int x = 0; bool first = true;
    for (const std::wstring& name : GlyphNames(chord)) { x += (first ? 0 : height / 5 + height / 3) + TextWidth(dc, height, name) + height / 5; first = false; }
    return x;
}
int Draw(HDC dc, int x, int y, int height, const Chord& chord, int) {
    bool first = true;
    for (const std::wstring& name : GlyphNames(chord)) {
        if (!first) x += height / 5 + height / 3;
        const int width = TextWidth(dc, height, name); Box(dc, x, y, width, height, name, RGB(140, 200, 150)); x += width + height / 5; first = false;
    }
    return x;
}
int KeyWidth(HDC dc, int height, const wchar_t* caption) { return TextWidth(dc, height, caption); }
int DrawKey(HDC dc, int x, int y, int height, const wchar_t* caption) { const int width = TextWidth(dc, height, caption); Box(dc, x, y, width, height, caption, RGB(238, 234, 226)); return x + width; }
int StickWidth(int height, bool) { return height + height * 3 / 5; }
int DrawStick(HDC dc, int x, int y, int height, int way, bool) {
    static const wchar_t* const kWays[4] = {L"L\x2191", L"L\x2193", L"L\x2190", L"L\x2192"};
    Box(dc, x, y, StickWidth(height, false), height, kWays[way & 3], RGB(150, 180, 230)); return x + StickWidth(height, false);
}
}
// Stand-in for the grab module: grabs on, one move picked.
// Stand-in grab moves: two MoTian grabs and an unsupported one, one MuXiaoKui grab.
std::vector<MotianGrab::Variant> StubGrabs() {
    std::vector<MotianGrab::Variant> out;
    const auto add = [&](const wchar_t* id, const wchar_t* boss, const wchar_t* name, bool supported) {
        MotianGrab::Variant v; v.index = static_cast<int32_t>(out.size()); v.id = id; v.boss_id = boss; v.boss_name = boss; v.name = name; v.supported = supported;
        out.push_back(v);
    };
    add(L"02", L"MoTian", L"Grab 02", true);
    add(L"MoTian:GA_MoTian_GEx_01:BD_MoTian_GEx_01:BP_BulletEvent_Grab_C_0", L"MoTian", L"Grab 01", true);
    add(L"MoTian:GA_MoTian_GEx_09", L"MoTian", L"Grab 09", false);
    add(L"MuXiaoKui:GA_MuXiaoKui_GEx_01", L"MuXiaoKui", L"Grab 01", true);
    return out;
}
namespace MotianGrab {
Snapshot GetSnapshot() {
    Snapshot s; s.options.enabled = true; s.options.variant = 0; s.variants = StubGrabs();
    return s;
}
std::string PreviewMontage(int32_t variant) { return variant >= 0 ? "/Stub/Grab_GEx_Montage.Grab_GEx_Montage" : ""; }
}

int CLSIDFor(const wchar_t* mime, CLSID* clsid) {
    UINT n = 0, size = 0; Gdiplus::GetImageEncodersSize(&n, &size);
    std::string buffer(size, '\0'); auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
    Gdiplus::GetImageEncoders(n, size, codecs);
    for (UINT i = 0; i < n; ++i) if (!wcscmp(codecs[i].MimeType, mime)) { *clsid = codecs[i].Clsid; return 1; }
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);  // a crash keeps the checks printed so far
    if (argc < 2) { std::printf("usage: combo_menu_preview <out.png> [skin] [width] [en]\n"); return 2; }
    skin_ = argc > 2 ? _wtoi(argv[2]) : 0;
    // Chains go to a .combo file of this run only (a fresh folder under %TEMP%), never next to the test.
    wchar_t temp[MAX_PATH]{}; GetTempPathW(MAX_PATH, temp);
    const std::wstring combos = std::wstring(temp) + L"pbz_combo_preview_" + std::to_wstring(GetCurrentProcessId());
    NpcCombo::SetFolder(combos);
    struct CleanCombos { std::wstring folder; ~CleanCombos() {
        WIN32_FIND_DATAW d{}; const HANDLE f = FindFirstFileW((folder + L"\\*.combo").c_str(), &d);
        if (f != INVALID_HANDLE_VALUE) { do DeleteFileW((folder + L"\\" + d.cFileName).c_str()); while (FindNextFileW(f, &d)); FindClose(f); }
        RemoveDirectoryW(folder.c_str()); } } clean_combos{combos};
    const int width = argc > 3 ? _wtoi(argv[3]) : 980;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    UiTheme::Initialize();
    const bool english = argc > 4 && !wcscmp(argv[4], L"en");  // the page in English (typed searches in English too)
    if (english) UiI18n::SetLanguage(UiI18n::Language::English);

    NpcRetarget::SkinMoves moves; NpcRetarget::GetSkinMoves(skin_, moves);
    // The NPC key's chain: lead wait, two skills joined by a long interval (slowed), a dodge, a skill; one free
    // node; the hero's light attack wired to a skill and any dodge to the stick's dodge. Saved without places, so
    // the page lays it out the way it does text saved before the canvas.
    std::vector<std::string> picks;
    for (const auto& s : moves.skills) if (picks.size() < 3) picks.push_back(s);
    if (!moves.dodges.empty()) picks.insert(picks.begin() + (std::min<size_t>)(2, picks.size()), moves.dodges.front());
    std::vector<NpcCombo::Node> nodes{{NpcCombo::NodeType::Interval, {}, 300}};
    for (size_t i = 0; i < picks.size(); ++i) {
        nodes.push_back({NpcCombo::NodeType::Skill, picks[i], 0});
        if (i == 0) nodes.push_back({NpcCombo::NodeType::Interval, {}, 2400});
        if (i == 1) nodes.push_back({NpcCombo::NodeType::Interval, {}, 400});
    }
    NpcCombo::Config config = NpcCombo::Linked(nodes);
    config.chain.push_back({NpcCombo::NodeType::Skill, moves.skills.size() > 3 ? moves.skills[3] : moves.skills[0], 0});
    const int free_index = static_cast<int>(config.chain.size()) - 1;
    const auto skill = [&](size_t i) { return moves.skills.size() > i ? moves.skills[i] : moves.skills[0]; };
    const int light_index = static_cast<int>(config.chain.size());
    NpcCombo::Node light = NpcCombo::TriggerNode(NpcCombo::Trigger::Light, 0, 0); light.next = light_index + 1;
    config.chain.push_back(light); config.chain.push_back({NpcCombo::NodeType::Skill, skill(4), 0});
    // The light chain goes on through a branch: light -> a move, heavy -> (unwired), light after a 2 s pause -> another.
    const int fork_index = static_cast<int>(config.chain.size());
    NpcCombo::Node fork = NpcCombo::BranchNode(0, 0);
    fork.routes = {{static_cast<int>(NpcCombo::Trigger::Light), 0, fork_index + 1}, {static_cast<int>(NpcCombo::Trigger::Heavy), 0, -1}, {static_cast<int>(NpcCombo::Trigger::Light), 2000, fork_index + 2}};
    config.chain[static_cast<size_t>(light_index) + 1].next = fork_index;
    config.chain.push_back(fork); config.chain.push_back({NpcCombo::NodeType::Skill, skill(5), 0}); config.chain.push_back({NpcCombo::NodeType::Skill, skill(6), 0});
    // Arrows: the light trigger at a 加速 (its whole chain), the 2 s row's move at a 霸体, the key at a 武器属性.
    const auto modifier = [&](NpcCombo::NodeType type, int from) {
        config.chain[static_cast<size_t>(from)].mods[static_cast<size_t>(NpcCombo::SlotOf(type))] = static_cast<int>(config.chain.size());
        config.chain.push_back(NpcCombo::ModifierNode(type, 0, 0));
    };
    modifier(NpcCombo::NodeType::Speed, light_index);
    modifier(NpcCombo::NodeType::Armor, fork_index + 2);
    modifier(NpcCombo::NodeType::Weapon, NpcCombo::KeyTrigger(config));
    NpcCombo::Node dodge = NpcCombo::TriggerNode(NpcCombo::Trigger::AnyDodge, 0, 0); dodge.next = static_cast<int>(config.chain.size()) + 1;
    config.chain.push_back(dodge); config.chain.push_back({NpcCombo::NodeType::AutoDodge});
    if (!picks.empty()) NpcCombo::RecordLength(picks[0], 1.6f);
    if (picks.size() > 1) NpcCombo::RecordLength(picks[1], 1.1f);
    config.placed = false;
    NpcCombo::Save(moves.profile, config);
    NpcCombo::Publish(moves.profile, true, 3, L"派生 2/4");

    WNDCLASSW cls{}; cls.lpfnWndProc = DefWindowProcW; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = L"ComboPreviewHost";
    cls.hbrBackground = UiTheme::Background(); RegisterClassW(&cls);
    host_ = CreateWindowExW(0, cls.lpszClassName, L"preview", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 20, 20, width + 16, argc > 5 ? _wtoi(argv[5]) : 980, nullptr, nullptr, cls.hInstance, nullptr);
    page_ = ComboMenu::CreatePage(host_);
    RECT r{}; GetClientRect(host_, &r);
    SetWindowPos(page_, nullptr, 0, 0, r.right, (std::max)(static_cast<LONG>(min_height_), r.bottom), SWP_NOZORDER);
    for (int i = 0; i < 20; ++i) {
        MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        ComboMenu::Tick(); Sleep(30);
    }
    GetClientRect(page_, &r);
    const auto shot = [&](const std::wstring& path) {
        RECT area{}; GetClientRect(page_, &area);
        HDC screen = GetDC(page_), dc = CreateCompatibleDC(screen);
        HBITMAP bitmap = CreateCompatibleBitmap(screen, area.right, area.bottom);
        const HGDIOBJ old = SelectObject(dc, bitmap);
        SendMessageW(page_, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
        { Gdiplus::Bitmap image(bitmap, nullptr); CLSID png{}; if (CLSIDFor(L"image/png", &png)) image.Save(path.c_str(), &png, nullptr); }
        SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc); ReleaseDC(page_, screen);
    };
    shot(argv[1]);
    std::printf("skin=%d profile=%s dodges=%zu skills=%zu size=%ldx%ld\n", skin_, moves.profile.c_str(), moves.dodges.size(), moves.skills.size(), r.right, r.bottom);

    // Scripted mouse checks on the canvas (messages straight to the page, as the hub would deliver them).
    int failures = 0;
    // A failed check also saves the page as <out>_fail<n>.png.
    const auto check = [&](bool ok, const char* what) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
        if (ok) return;
        ++failures; std::wstring path = argv[1]; path.insert(path.size() - 4, L"_fail" + std::to_wstring(failures)); shot(path);
    };
    const auto send = [&](UINT message, POINT p, WPARAM w = 0) { SendMessageW(page_, message, w, MAKELPARAM(p.x, p.y)); };
    const auto drag = [&](POINT a, POINT b) {
        send(WM_LBUTTONDOWN, a, MK_LBUTTON);
        for (int i = 1; i <= 8; ++i) send(WM_MOUSEMOVE, {a.x + (b.x - a.x) * i / 8, a.y + (b.y - a.y) * i / 8}, MK_LBUTTON);
        send(WM_LBUTTONUP, b);
    };
    const auto click = [&](POINT a) { send(WM_LBUTTONDOWN, a, MK_LBUTTON); send(WM_LBUTTONUP, a); };
    const auto now = [&] { return NpcCombo::Load(moves.profile); };
    const RECT canvas = ComboMenu::Probe::Canvas();
    SendMessageW(page_, WM_KEYDOWN, VK_HOME, 0);  // every node in view before pointing at them
    const int key = NpcCombo::KeyTrigger(now()), free_node = free_index, tail = NpcCombo::Tail(now(), key);
    const size_t wired = NpcCombo::Order(now(), key).size();
    // Bare canvas (no node, no tool) with room around it, found now (the view moves as the checks go).
    const auto bare = [&] {
        for (int y = canvas.bottom - 40; y > canvas.top + 40; y -= 20)
            for (int x = canvas.right - 40; x > canvas.left + 140; x -= 20) {
                bool ok = true;
                for (int dy = -20; dy <= 60 && ok; dy += 20) for (int dx = -120; dx <= 20 && ok; dx += 20) ok = ComboMenu::Probe::Empty({x + dx, y + dy});
                if (ok) return POINT{x, y};
            }
        return POINT{canvas.right - 30, canvas.top + 30};
    };
    POINT empty = bare();
    const auto type = [&](const wchar_t* text) { for (const wchar_t* c = text; *c; ++c) SendMessageW(page_, WM_CHAR, *c, 0); };
    const auto key_down = [&](WPARAM k) { SendMessageW(page_, WM_KEYDOWN, k, 0); };

    {
        // 预输入: the chain's fixed card (top-left after 整理): + / − by 50 ms, a typed value (0: 不清除), never removed.
        const int pre = NpcCombo::PreInputNode(now());
        check(pre >= 0 && NpcCombo::PreInputMs(now()) == NpcCombo::kPreInputDefault && ComboMenu::Probe::NodeRect(pre).bottom < ComboMenu::Probe::NodeRect(key).top &&
              ComboMenu::Probe::NodeRect(pre).left <= ComboMenu::Probe::NodeRect(key).left, "the chain has its 预输入 node (0.3 s), on its own row at the top-left");
        if (pre >= 0) {
            ComboMenu::Probe::Show(pre);
            click(ComboMenu::Probe::Plus(pre)); click(ComboMenu::Probe::Plus(pre)); click(ComboMenu::Probe::Minus(pre));
            check(NpcCombo::PreInputMs(now()) == NpcCombo::kPreInputDefault + NpcCombo::kPreInputStep, "预输入 + / − step 50 ms");
            click(ComboMenu::Probe::Value(pre)); type(L"0"); key_down(VK_RETURN);
            check(NpcCombo::PreInputMs(now()) == 0, "预输入 typed 0: 不清除");
            { std::wstring path = argv[1]; path.insert(path.size() - 4, L"_preinput"); shot(path); }
            click(ComboMenu::Probe::Value(pre)); type(L"9"); key_down(VK_RETURN);
            check(NpcCombo::PreInputMs(now()) == NpcCombo::kPreInputMax, "预输入 typed 9 s: clamped to 5 s");
            click(ComboMenu::Probe::Value(pre)); type(L"0.3"); key_down(VK_RETURN);
            const size_t pre_nodes = now().chain.size();
            click(ComboMenu::Probe::Header(pre)); key_down(VK_DELETE);
            check(NpcCombo::PreInputNode(now()) == pre && now().chain.size() == pre_nodes && NpcCombo::PreInputMs(now()) == 300, "Delete leaves 预输入 (a notice says why)");
        }
        SendMessageW(page_, WM_KEYDOWN, VK_HOME, 0);
    }
    const NpcCombo::Node before = now().chain[static_cast<size_t>(free_node)];
    ComboMenu::Probe::Show(free_node);
    POINT head = ComboMenu::Probe::Header(free_node);
    drag(head, {head.x + 120, head.y + 50});
    const NpcCombo::Node after = now().chain[static_cast<size_t>(free_node)];
    check(after.x > before.x + 60 && after.y > before.y + 20, "dragging a node's header moves it (and saves)");

    ComboMenu::Probe::Put(free_node, tail); ComboMenu::Probe::Show(tail);
    drag(ComboMenu::Probe::Pin(tail, true), ComboMenu::Probe::Header(free_node));
    check(now().chain[static_cast<size_t>(tail)].next == free_node && NpcCombo::Order(now(), key).size() == wired + 1, "a wire from the last node's output to the free node joins it to the chain");
    empty = bare();
    drag(ComboMenu::Probe::Pin(tail, true), empty);
    check(now().chain[static_cast<size_t>(tail)].next == -1 && ComboMenu::Probe::SearchOpen(), "a wire dropped on empty canvas unwires and opens the search");
    key_down(VK_ESCAPE);
    check(!ComboMenu::Probe::SearchOpen() && now().chain[static_cast<size_t>(tail)].next == -1, "Esc closes the search, nothing added");
    ComboMenu::Probe::Show(tail);
    drag(ComboMenu::Probe::Pin(free_node, false), ComboMenu::Probe::Header(tail));
    check(now().chain[static_cast<size_t>(tail)].next == free_node, "a wire dragged back from an input pin wires that node in");

    const size_t count = now().chain.size();
    drag(ComboMenu::Probe::Pill(0), {canvas.right - 260, canvas.bottom - 80});
    check(now().chain.size() == count + 1 && now().chain.back().next == -1, "a move dragged from the list onto the canvas is a new free node");
    const int dropped = static_cast<int>(now().chain.size()) - 1;
    click(ComboMenu::Probe::Pill(1));
    check(now().chain.size() == count + 2 && now().chain[static_cast<size_t>(dropped)].next == dropped + 1, "clicking a move appends it after the selected node, wired");
    // 招式预览: the mouse resting on a move pill for 0.4 s asks for its preview (the boss blueprint and the move's
    // package); its frame shows in a card by the pill; the mouse leaving the move hides it.
    {
        send(WM_MOUSEMOVE, ComboMenu::Probe::Pill(1)); ComboMenu::Tick();
        check(preview_shown_.empty(), "招式预览: nothing at once");
        Sleep(450); ComboMenu::Tick(); ComboMenu::Tick();
        check(preview_shown_.find("/GA_") != std::string::npos, "招式预览: a pill the mouse rests on asks for its move's package after 0.4 s");
        { std::wstring path = argv[1]; path.insert(path.size() - 4, L"_preview"); shot(path); }
        send(WM_MOUSEMOVE, {canvas.left + 4, canvas.bottom - 4}); ComboMenu::Tick();
        check(preview_shown_.empty(), "招式预览: hidden once the mouse leaves the move");
    }

    const POINT key_before = ComboMenu::Probe::Header(-1);
    empty = bare();
    drag(empty, {empty.x - 100, empty.y + 40});
    const POINT key_after = ComboMenu::Probe::Header(-1);
    std::printf("pan from %ld,%ld key %ld,%ld -> %ld,%ld\n", empty.x, empty.y, key_before.x, key_before.y, key_after.x, key_after.y);
    check(std::abs(key_after.x - (key_before.x - 100)) <= 2 && std::abs(key_after.y - (key_before.y + 40)) <= 2, "dragging empty canvas pans the view");

    const int zoom = ComboMenu::Probe::Zoom();
    POINT corner{canvas.right - 24, canvas.bottom - 24}; ClientToScreen(page_, &corner);  // over an interval's value the wheel sets the interval
    SendMessageW(page_, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM(corner.x, corner.y));
    check(ComboMenu::Probe::Zoom() > zoom, "the wheel over the canvas zooms in");

    ComboMenu::Probe::Show(dropped);
    click(ComboMenu::Probe::Header(dropped));
    key_down(VK_DELETE);
    check(now().chain.size() == count + 1 && now().chain[static_cast<size_t>(tail)].next == free_node, "Delete removes the selected node, other wires stay");
    ComboMenu::Probe::Show(key);
    click(ComboMenu::Probe::Header(-1)); key_down(VK_DELETE);
    check(NpcCombo::KeyTrigger(now()) == key, "the NPC key node cannot be deleted");

    click(ComboMenu::Probe::Tool(2));
    const int added = static_cast<int>(now().chain.size()) - 1;
    check(now().chain.size() == count + 2 && now().chain.back().type == NpcCombo::NodeType::Trigger && now().chain.back().trigger == static_cast<int>(NpcCombo::Trigger::Light),
          "+ 替换按键 adds a hero light attack trigger");
    ComboMenu::Probe::Show(added);
    click(ComboMenu::Probe::Field(added));
    check(ComboMenu::Probe::SearchOpen() && ComboMenu::Probe::SearchRows() == static_cast<size_t>(NpcCombo::Trigger::Count), "clicking the trigger's field lists the hero's buttons (a header and 17, 格挡 and 简易处决 among them)");
    key_down(VK_DOWN); key_down(VK_RETURN);
    check(!ComboMenu::Probe::SearchOpen() && now().chain[static_cast<size_t>(added)].trigger == static_cast<int>(NpcCombo::Trigger::Heavy), "the list starts on the field's button; Down + Enter picks heavy attack");
    click(ComboMenu::Probe::Tool(1));
    check(now().chain.size() == count + 3 && now().chain[static_cast<size_t>(added)].next == added + 1 && now().chain.back().type == NpcCombo::NodeType::Interval,
          "+ 间隔 appends after the selected trigger, wired");

    key_down(VK_TAB);
    check(ComboMenu::Probe::SearchOpen() && ComboMenu::Probe::SearchRows() > 10, "Tab opens the search with every item");
    const size_t all_rows = ComboMenu::Probe::SearchRows();
    type(L"skill");
    check(ComboMenu::Probe::SearchRows() > 0 && ComboMenu::Probe::SearchRows() < all_rows, "typing filters the search");
    { std::wstring path = argv[1]; path.insert(path.size() - 4, L"_search"); shot(path); }
    // 招式预览 in the search box: a move row picked with the keys, or under the mouse, shows its preview after 0.4 s in a
    // card by the box (never over its list).
    {
        ComboMenu::Tick(); Sleep(450); ComboMenu::Tick();
        check(preview_shown_.empty(), "搜索框招式预览: none for the row the box opens on");
        const int first = ComboMenu::Probe::SearchSelected();
        key_down(VK_DOWN); key_down(VK_UP);
        check(ComboMenu::Probe::SearchSelected() == first, "搜索框: Down, Up is back on the first match");
        ComboMenu::Tick(); Sleep(450); ComboMenu::Tick(); ComboMenu::Tick();
        const std::string keyed = preview_shown_;
        check(keyed.find("/GA_") != std::string::npos, "搜索框招式预览: the move row picked with the keys asks for its package after 0.4 s");
        { std::wstring path = argv[1]; path.insert(path.size() - 4, L"_searchpreview"); shot(path); }
        RECT overlap{}; const RECT card = ComboMenu::Probe::PreviewCard(), box = ComboMenu::Probe::SearchBox();
        check(card.right > card.left && !IntersectRect(&overlap, &card, &box), "搜索框招式预览: the card is by the box, not over it");
        int other = -1;
        for (int i = first + 1; i < static_cast<int>(ComboMenu::Probe::SearchRows()) && other < 0; ++i) if (ComboMenu::Probe::SearchRow(i).x >= 0) other = i;
        if (other >= 0) {
            send(WM_MOUSEMOVE, ComboMenu::Probe::SearchRow(other)); ComboMenu::Tick();
            check(preview_shown_.empty(), "搜索框招式预览: the mouse on another row starts over");
            Sleep(450); ComboMenu::Tick(); ComboMenu::Tick();
            check(!preview_shown_.empty() && preview_shown_ != keyed, "搜索框招式预览: the row under the mouse shows its own move");
            send(WM_MOUSEMOVE, {canvas.left + 4, canvas.bottom - 4}); ComboMenu::Tick();  // off the box and every move: no card over what comes next
        }
    }
    key_down(VK_RETURN);
    check(!ComboMenu::Probe::SearchOpen() && now().chain.size() == count + 4 && now().chain.back().type == NpcCombo::NodeType::Skill && now().chain.back().next == -1,
          "Enter adds the first match as a free node");
    empty = bare();
    send(WM_RBUTTONDOWN, empty, MK_RBUTTON); send(WM_RBUTTONUP, empty);
    check(ComboMenu::Probe::SearchOpen(), "a right-click on the canvas opens the search");
    key_down(VK_ESCAPE);
    ComboMenu::Probe::Show(free_node);
    empty = bare();
    drag(ComboMenu::Probe::Pin(free_node, true), empty);
    { const std::string first = moves.skills.front(); type(std::wstring(first.begin(), first.end()).c_str()); }  // a name every skin has
    key_down(VK_RETURN);
    {
        const NpcCombo::Config c = now(); const int fresh = static_cast<int>(c.chain.size()) - 1;
        check(c.chain.size() == count + 5 && c.chain[static_cast<size_t>(free_node)].next == fresh &&
              (c.chain.back().type == NpcCombo::NodeType::Skill || c.chain.back().type == NpcCombo::NodeType::AutoDodge), "a wire dropped on empty canvas, searched and picked, wires to the new node");
    }
    // 按键分支: wire its unwired heavy row, pick its button, its pause, a row more.
    const int fork_node = fork_index;
    ComboMenu::Probe::Show(fork_node);
    { std::wstring path = argv[1]; path.insert(path.size() - 4, L"_branch"); shot(path); }
    ComboMenu::Probe::Put(free_node, fork_node); ComboMenu::Probe::Show(fork_node);
    drag(ComboMenu::Probe::RouteOut(fork_node, 1), ComboMenu::Probe::Header(free_node));
    check(now().chain[static_cast<size_t>(fork_node)].routes[1].next == free_node, "a wire from the heavy row's own pin wires that row");
    click(ComboMenu::Probe::RouteField(fork_node, 1));
    check(ComboMenu::Probe::SearchOpen() && ComboMenu::Probe::SearchRows() == static_cast<size_t>(NpcCombo::Trigger::Count) - 1,
          "a row's field lists the buttons, the NPC key, 自定义组合键 and 自定义按键 (no 主角处决 / 主角简易处决: no press of a branch)");
    key_down(VK_UP); key_down(VK_RETURN);
    check(now().chain[static_cast<size_t>(fork_node)].routes[1].button == static_cast<int>(NpcCombo::Trigger::Light), "Up + Enter: the row is now light attack");
    click(ComboMenu::Probe::RoutePlus(fork_node, 1));
    check(now().chain[static_cast<size_t>(fork_node)].routes[1].pause_ms == NpcCombo::kPauseStep, "the row's + adds a 0.1 s pause");
    click(ComboMenu::Probe::RouteAdd(fork_node));
    check(now().chain[static_cast<size_t>(fork_node)].routes.size() == 4, "+ 添加按键 adds a row");
    drag(ComboMenu::Probe::RouteOut(fork_node, 3), bare());
    check(ComboMenu::Probe::SearchOpen(), "the new row's wire dropped on empty canvas opens the search");
    type(L"GA_"); key_down(VK_RETURN);
    { const NpcCombo::Config c = now(); check(c.chain[static_cast<size_t>(fork_node)].routes[3].next == static_cast<int>(c.chain.size()) - 1, "the picked move is wired from that row"); }
    // A row of 自定义组合键 / 自定义按键: its keys and mode on a line under it, picked (or recorded) as a trigger's.
    {
        const auto row3 = [&] { return now().chain[static_cast<size_t>(fork_node)].routes[3]; };
        ComboMenu::Probe::Show(fork_node);
        const RECT plain_card = ComboMenu::Probe::NodeRect(fork_node);
        check(ComboMenu::Probe::RouteKeys(fork_node, 3).x < 0, "a plain row has no keys line");
        click(ComboMenu::Probe::RouteField(fork_node, 3));
        type(english ? L"custom key" : L"组合键"); key_down(VK_RETURN);
        const RECT keys_card = ComboMenu::Probe::NodeRect(fork_node);
        check(row3().button == static_cast<int>(NpcCombo::Trigger::Combo) && ComboMenu::Probe::SearchOpen() && ComboMenu::Probe::SearchRows() == 10 &&
              ComboMenu::Probe::RouteKeys(fork_node, 3).x >= 0 && keys_card.bottom - keys_card.top > plain_card.bottom - plain_card.top,
              "a row picked as 自定义组合键 grows a keys line and opens its keys at once (7 keys and 完成)");
        key_down(VK_RETURN); key_down(VK_DOWN); key_down(VK_RETURN); key_down(VK_ESCAPE);
        check(row3().keys == std::vector<int>{static_cast<int>(NpcCombo::Trigger::Light), static_cast<int>(NpcCombo::Trigger::Heavy)} &&
              NpcCombo::Encode(now()).find(L",14/1;2/0/400,0,") != std::wstring::npos, "its keys: 轻攻击 then 重攻击, saved in the row (14/1;2/0/400)");
        { std::wstring path = argv[1]; path.insert(path.size() - 4, L"_branchkeys"); shot(path); }
        click(ComboMenu::Probe::RouteMode(fork_node, 3));
        check(ComboMenu::Probe::SearchOpen() && ComboMenu::Probe::SearchRows() == 10, "its mode field lists 一起按 and seven gaps in order");
        type(L"650"); key_down(VK_RETURN);
        check(!row3().chord && row3().window == 650 && NpcCombo::Encode(now()).find(L",14/1;2/0/650,0,") != std::wstring::npos, "a typed 650: in order within 650 ms");
        click(ComboMenu::Probe::RouteField(fork_node, 3));
        type(english ? L"custom keys" : L"自定义按键"); key_down(VK_RETURN);
        check(row3().button == static_cast<int>(NpcCombo::Trigger::Keys) && row3().keys.empty() && ComboMenu::Probe::SearchOpen() && ComboMenu::Probe::SearchRows() == 25,
              "as 自定义按键 its old keys go and its key list opens (录制, 16 pad buttons, 4 left-stick ways, 完成)");
        key_down(VK_RETURN);
        check(ComboMenu::Probe::RecordingNode() == fork_node && ComboMenu::Probe::RecordingRoute() == 3, "Enter records the row's keys");
        key_down('Q'); key_down(VK_RETURN);
        check(ComboMenu::Probe::RecordingNode() < 0 && row3().keys == std::vector<int>{'Q'} && NpcCombo::Encode(now()).find(L",15/81/0/650/0,0,") != std::wstring::npos,
              "Q recorded into the row; Enter confirms it (15/81/0/650/0)");
        click(ComboMenu::Probe::RouteMode(fork_node, 3));
        check(ComboMenu::Probe::SearchOpen() && ComboMenu::Probe::SearchRows() == 13, "a 自定义按键 row's mode also offers 吞掉 / 不吞掉");
        type(english ? L"swallows" : L"吞掉原动作"); key_down(VK_RETURN);
        check(row3().swallow && NpcCombo::Encode(now()).find(L",15/81/0/650/1,0,") != std::wstring::npos, "吞掉 picked there is saved (/1)");
        click(ComboMenu::Probe::RouteField(fork_node, 3));
        type(english ? L"hero light" : L"主角轻攻击"); key_down(VK_RETURN);
        const RECT back_card = ComboMenu::Probe::NodeRect(fork_node);
        check(row3().button == static_cast<int>(NpcCombo::Trigger::Light) && row3().keys.empty() && ComboMenu::Probe::RouteKeys(fork_node, 3).x < 0 && !ComboMenu::Probe::SearchOpen() &&
              back_card.bottom - back_card.top == plain_card.bottom - plain_card.top, "back to 轻攻击: no keys line, the card its old size");
    }
    // + 按键分支 after the selected move; 加速 / 霸体 / 武器属性 and where each may point.
    ComboMenu::Probe::Show(free_node); click(ComboMenu::Probe::Header(free_node));
    click(ComboMenu::Probe::Tool(7));
    { const NpcCombo::Config c = now(); check(c.chain.back().type == NpcCombo::NodeType::Branch && c.chain[static_cast<size_t>(free_node)].next == static_cast<int>(c.chain.size()) - 1, "+ 按键分支 appends a branch after the selected move"); }
    const auto mod = [&](int node, int slot) { return now().chain[static_cast<size_t>(node)].mods[static_cast<size_t>(slot)]; };
    ComboMenu::Probe::Show(key); click(ComboMenu::Probe::Header(-1));
    click(ComboMenu::Probe::Tool(8));
    const int speed_node = static_cast<int>(now().chain.size()) - 1;
    check(now().chain.back().type == NpcCombo::NodeType::Speed && mod(key, NpcCombo::kModSpeed) == speed_node, "+ 加速 with the key node selected: the key points at it (the whole chain)");
    ComboMenu::Probe::Show(speed_node); click(ComboMenu::Probe::Plus(speed_node));
    check(now().chain[static_cast<size_t>(speed_node)].speed == NpcCombo::kSpeedNew + NpcCombo::kSpeedStep, "the 加速 + raises its multiplier by 0.1");
    click(ComboMenu::Probe::Value(speed_node));
    check(ComboMenu::Probe::Editing(), "clicking the multiplier starts typing");
    { std::wstring path = argv[1]; path.insert(path.size() - 4, L"_typing"); type(L"2.2"); shot(path); type(L"5"); }
    key_down(VK_RETURN);
    check(!ComboMenu::Probe::Editing() && now().chain[static_cast<size_t>(speed_node)].speed == 225, "typing 2.25 + Enter sets ×2.25");
    click(ComboMenu::Probe::Value(speed_node)); type(L"9"); key_down(VK_RETURN);
    check(now().chain[static_cast<size_t>(speed_node)].speed == 900, "a typed ×9 sets ×9.00");
    click(ComboMenu::Probe::Value(speed_node)); type(L"12"); key_down(VK_RETURN);
    check(now().chain[static_cast<size_t>(speed_node)].speed == NpcCombo::kSpeedMax && NpcCombo::kSpeedMax == 1000, "a typed ×12 is held to ×10.00");
    click(ComboMenu::Probe::Value(speed_node)); type(L"1.7"); key_down(VK_ESCAPE);
    check(!ComboMenu::Probe::Editing() && now().chain[static_cast<size_t>(speed_node)].speed == NpcCombo::kSpeedMax, "Esc drops what was typed");
    click(ComboMenu::Probe::Value(speed_node)); type(L"1.5"); click(ComboMenu::Probe::Header(speed_node));
    check(!ComboMenu::Probe::Editing() && now().chain[static_cast<size_t>(speed_node)].speed == 150, "a click elsewhere keeps what was typed");
    {  // 曲线调节: the checkbox, 编辑曲线… and its window (drag a point, double-click adds one, right-click removes it)
        click(ComboMenu::Probe::Toggle(speed_node));
        check(now().chain[static_cast<size_t>(speed_node)].curve && now().chain[static_cast<size_t>(speed_node)].points.size() == 3 && !SpeedCurveWindow::Window(),
              "曲线调节 ticked: a curve of three points, no window yet");
        click(ComboMenu::Probe::Field(speed_node));
        const HWND curve = SpeedCurveWindow::Window();
        check(curve && SpeedCurveWindow::EditedNode() == speed_node, "编辑曲线… opens the curve window for that node");
        if (curve) {
            const auto csend = [&](UINT message, POINT p, WPARAM w = 0) { SendMessageW(curve, message, w, MAKELPARAM(p.x, p.y)); };
            const POINT mid = SpeedCurveWindow::PointAt(1), up{mid.x + 40, mid.y - 60};
            csend(WM_MOUSEMOVE, mid); csend(WM_LBUTTONDOWN, mid, MK_LBUTTON);
            for (int i = 1; i <= 6; ++i) csend(WM_MOUSEMOVE, {mid.x + (up.x - mid.x) * i / 6, mid.y + (up.y - mid.y) * i / 6}, MK_LBUTTON);
            csend(WM_LBUTTONUP, up);
            const NpcCombo::Node dragged = now().chain[static_cast<size_t>(speed_node)];
            check(dragged.points.size() == 3 && dragged.points[1].t > 50 && dragged.points[1].speed > 300 && dragged.points[1].speed % 5 == 0,
                  "dragging the middle point right and up saves it later and faster (5 % steps)");
            const RECT g = SpeedCurveWindow::Graph();
            const POINT add{g.left + (g.right - g.left) / 5, (g.top + g.bottom) / 2};
            csend(WM_MOUSEMOVE, add); csend(WM_LBUTTONDBLCLK, add, MK_LBUTTON); csend(WM_LBUTTONUP, add);
            check(now().chain[static_cast<size_t>(speed_node)].points.size() == 4 && now().chain[static_cast<size_t>(speed_node)].points[1].t == 20, "a double-click adds a point at 20 %");
            { std::wstring path = argv[1]; path.insert(path.size() - 4, L"_curve");
              RECT area{}; GetClientRect(curve, &area); HDC screen = GetDC(curve), dc = CreateCompatibleDC(screen);
              HBITMAP bitmap = CreateCompatibleBitmap(screen, area.right, area.bottom); const HGDIOBJ old = SelectObject(dc, bitmap);
              PrintWindow(curve, dc, PW_CLIENTONLY);
              { Gdiplus::Bitmap image(bitmap, nullptr); CLSID png{}; if (CLSIDFor(L"image/png", &png)) image.Save(path.c_str(), &png, nullptr); }
              SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc); ReleaseDC(curve, screen); }
            const POINT second = SpeedCurveWindow::PointAt(1);
            csend(WM_MOUSEMOVE, second); csend(WM_RBUTTONUP, second);
            const POINT first = SpeedCurveWindow::PointAt(0);
            csend(WM_MOUSEMOVE, first); csend(WM_RBUTTONUP, first);
            check(now().chain[static_cast<size_t>(speed_node)].points.size() == 3 && now().chain[static_cast<size_t>(speed_node)].points[0].t == 0,
                  "a right-click removes a point; the ends stay");
            shot([&] { std::wstring path = argv[1]; path.insert(path.size() - 4, L"_curvenode"); return path; }());
            SpeedCurveWindow::Close();
            check(!SpeedCurveWindow::Window(), "the curve window closes");
        }
        click(ComboMenu::Probe::Toggle(speed_node));
        check(!now().chain[static_cast<size_t>(speed_node)].curve && now().chain[static_cast<size_t>(speed_node)].points.size() == 3 && now().chain[static_cast<size_t>(speed_node)].speed == 150,
              "曲线调节 off again: the one multiplier, the curve kept");
    }
    // A move's 派生点: − / + by 0.05 s, typed in seconds (0: when it ends), at most 10 s.
    ComboMenu::Probe::Show(free_node);
    check(now().chain[static_cast<size_t>(free_node)].ms == 0, "a new move has no 派生点 (cut when it ends)");
    click(ComboMenu::Probe::Plus(free_node));
    check(now().chain[static_cast<size_t>(free_node)].ms == NpcCombo::kCancelStep, "a move's 派生点 + adds 0.05 s");
    click(ComboMenu::Probe::Minus(free_node)); click(ComboMenu::Probe::Minus(free_node));
    check(now().chain[static_cast<size_t>(free_node)].ms == 0, "its − stops at 0");
    click(ComboMenu::Probe::Value(free_node));
    check(ComboMenu::Probe::Editing(), "clicking a move's 派生点 starts typing");
    type(L"0.4"); key_down(VK_RETURN);
    check(!ComboMenu::Probe::Editing() && now().chain[static_cast<size_t>(free_node)].ms == 400, "typing 0.4 + Enter: the next move may cut it 0.4 s in");
    click(ComboMenu::Probe::Value(free_node)); type(L"25"); key_down(VK_RETURN);
    check(now().chain[static_cast<size_t>(free_node)].ms == NpcCombo::kCancelMax, "a typed 25 s is held to 10 s");
    click(ComboMenu::Probe::Value(free_node)); type(L"0.35"); key_down(VK_RETURN);
    check(now().chain[static_cast<size_t>(free_node)].ms == 350 && NpcCombo::Encode(now()).find(L",350|") != std::wstring::npos, "0.35 s, saved with the move");
    { std::wstring path = argv[1]; path.insert(path.size() - 4, L"_cut"); shot(path); }
    // A move joins the same 加速 with an arrow from its round pin; one 加速, many nodes.
    ComboMenu::Probe::Put(free_node, speed_node); ComboMenu::Probe::Show(speed_node);
    drag(ComboMenu::Probe::ModOut(free_node), ComboMenu::Probe::Header(speed_node));
    check(mod(free_node, NpcCombo::kModSpeed) == speed_node && NpcCombo::PointersTo(now(), speed_node).nodes == 2, "an arrow from a move's round pin onto 加速: the move joins it");
    { const NpcCombo::Config c = now(); check(NpcCombo::SpeedFor(c, -1, free_node) == 150, "that move plays at ×1.50"); }
    click(ComboMenu::Probe::Header(speed_node)); click(ComboMenu::Probe::Tool(10));
    const int armor_node = static_cast<int>(now().chain.size()) - 1;
    check(now().chain.back().type == NpcCombo::NodeType::Armor && NpcCombo::PointersTo(now(), armor_node).nodes == 0, "+ 霸体 with a modifier selected: nothing points at it yet");
    ComboMenu::Probe::Put(armor_node, free_node); ComboMenu::Probe::Show(free_node);
    drag(ComboMenu::Probe::ModIn(armor_node), ComboMenu::Probe::Header(free_node));
    check(mod(free_node, NpcCombo::kModArmor) == armor_node && mod(free_node, NpcCombo::kModSpeed) == speed_node, "an arrow dragged back from 霸体's pin onto the move: it points at 霸体 too, keeping its 加速");
    click(ComboMenu::Probe::Tool(9));
    const int weapon_node = static_cast<int>(now().chain.size()) - 1;
    ComboMenu::Probe::Put(weapon_node, free_node); ComboMenu::Probe::Show(free_node);
    drag(ComboMenu::Probe::ModOut(free_node), ComboMenu::Probe::Header(weapon_node));
    check(now().chain.back().type == NpcCombo::NodeType::Weapon && mod(free_node, NpcCombo::kModWeapon) == -1, "a move's arrow onto 武器属性 is refused");
    ComboMenu::Probe::Put(key, weapon_node); ComboMenu::Probe::Show(weapon_node);
    drag(ComboMenu::Probe::ModOut(key), ComboMenu::Probe::Header(weapon_node));
    check(mod(key, NpcCombo::kModWeapon) == weapon_node, "the key's arrow onto 武器属性 is taken (it replaces the one before)");
    ComboMenu::Probe::Show(free_node);
    drag(ComboMenu::Probe::ModOut(free_node), ComboMenu::Probe::Header(free_node));
    check(mod(free_node, NpcCombo::kModSpeed) == speed_node && mod(free_node, NpcCombo::kModArmor) == armor_node, "an arrow onto a node that is not a modifier changes nothing");
    drag(ComboMenu::Probe::ModOut(free_node), bare());
    check(ComboMenu::Probe::SearchOpen() && ComboMenu::Probe::SearchRows() == 9, "an arrow dropped on empty canvas lists the modifiers a move may point at (加速, 霸体, 无敌, 无视防御, 允许中断, 保持派生, 伤害倍率, 强制受击)");
    { std::wstring path = argv[1]; path.insert(path.size() - 4, L"_modsearch"); shot(path); }
    key_down(VK_RETURN);
    { const NpcCombo::Config c = now(); check(c.chain.back().type == NpcCombo::NodeType::Speed && mod(free_node, NpcCombo::kModSpeed) == static_cast<int>(c.chain.size()) - 1, "picking 加速 adds one the move points at"); }
    // 允许中断: + 允许中断 with a move selected points the move at it.
    ComboMenu::Probe::Show(free_node); click(ComboMenu::Probe::Header(free_node));
    click(ComboMenu::Probe::Tool(11));
    const int cut_node = static_cast<int>(now().chain.size()) - 1;
    check(now().chain.back().type == NpcCombo::NodeType::Interrupt && mod(free_node, NpcCombo::kModInterrupt) == cut_node, "+ 允许中断 with a move selected: the move points at it");
    { const NpcCombo::Config c = now(); check(NpcCombo::InterruptFor(c, -1, free_node) && mod(free_node, NpcCombo::kModArmor) == armor_node, "that move may be cut at any time; its other arrows stay"); }
    // 无敌: + 无敌 with a move selected points the move at it; the move keeps 霸体 and 允许中断.
    ComboMenu::Probe::Show(free_node); click(ComboMenu::Probe::Header(free_node));
    click(ComboMenu::Probe::Tool(21));
    {
        const int inv_node = static_cast<int>(now().chain.size()) - 1; const NpcCombo::Config c = now();
        check(c.chain.back().type == NpcCombo::NodeType::Invincible && mod(free_node, NpcCombo::kModInvincible) == inv_node && NpcCombo::InvincibleFor(c, -1, free_node) &&
              mod(free_node, NpcCombo::kModArmor) == armor_node && mod(free_node, NpcCombo::kModInterrupt) == cut_node, "+ 无敌 with a move selected: the move points at it, its 霸体 and 允许中断 stay");
        check(NpcCombo::Decode(NpcCombo::Encode(c)).chain[static_cast<size_t>(inv_node)].type == NpcCombo::NodeType::Invincible, "the page saves 无敌 and reads it back");
    }
    // 无视防御: + 无视防御 with a move selected points the move at it; its other arrows stay.
    ComboMenu::Probe::Show(free_node); click(ComboMenu::Probe::Header(free_node));
    click(ComboMenu::Probe::Tool(30));
    {
        const int guard_node = static_cast<int>(now().chain.size()) - 1; const NpcCombo::Config c = now();
        check(c.chain.back().type == NpcCombo::NodeType::IgnoreGuard && mod(free_node, NpcCombo::kModIgnoreGuard) == guard_node && NpcCombo::IgnoreGuardFor(c, -1, free_node) &&
              mod(free_node, NpcCombo::kModArmor) == armor_node && mod(free_node, NpcCombo::kModInterrupt) == cut_node, "+ 无视防御 with a move selected: the move points at it, its other arrows stay");
        check(NpcCombo::Decode(NpcCombo::Encode(c)).chain[static_cast<size_t>(guard_node)].type == NpcCombo::NodeType::IgnoreGuard, "the page saves 无视防御 and reads it back");
    }
    // 保持派生: + 保持派生 with a move selected points the move at it (3 s); its − takes 0.1 s off.
    ComboMenu::Probe::Show(free_node); click(ComboMenu::Probe::Header(free_node));
    click(ComboMenu::Probe::Tool(31));
    {
        const int keep_node = static_cast<int>(now().chain.size()) - 1; const NpcCombo::Config c = now();
        check(c.chain.back().type == NpcCombo::NodeType::KeepChain && c.chain.back().ms == NpcCombo::kKeepChainDefault && mod(free_node, NpcCombo::kModKeepChain) == keep_node &&
              NpcCombo::KeepChainFor(c, -1, free_node) == NpcCombo::kKeepChainDefault && mod(free_node, NpcCombo::kModArmor) == armor_node &&
              mod(free_node, NpcCombo::kModInterrupt) == cut_node, "+ 保持派生 with a move selected: the move points at it (3 s), its other arrows stay");
        ComboMenu::Probe::Show(keep_node); click(ComboMenu::Probe::Minus(keep_node));
        check(now().chain[static_cast<size_t>(keep_node)].ms == NpcCombo::kKeepChainDefault - NpcCombo::kKeepChainStep, "保持派生's − takes 0.1 s off its time");
        check(NpcCombo::Decode(NpcCombo::Encode(now())).chain[static_cast<size_t>(keep_node)].ms == NpcCombo::kKeepChainDefault - NpcCombo::kKeepChainStep,
              "the page saves 保持派生 with its time and reads it back");
    }
    // Every arrow ends in a head in its modifier's colour, just outside the card it points at, at 50 %, 100 % and 150 %,
    // and still over another card laid across it.
    const auto pixel = [&](POINT p) {
        RECT area{}; GetClientRect(page_, &area);
        HDC screen = GetDC(page_), dc = CreateCompatibleDC(screen);
        HBITMAP bitmap = CreateCompatibleBitmap(screen, area.right, area.bottom);
        const HGDIOBJ old = SelectObject(dc, bitmap);
        SendMessageW(page_, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
        const COLORREF c = GetPixel(dc, p.x, p.y);
        SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc); ReleaseDC(page_, screen);
        return c;
    };
    const auto close_to = [](COLORREF a, COLORREF b) {
        return std::abs(GetRValue(a) - GetRValue(b)) < 48 && std::abs(GetGValue(a) - GetGValue(b)) < 48 && std::abs(GetBValue(a) - GetBValue(b)) < 48;
    };
    const auto head_shows = [&](int node, int slot) {
        ComboMenu::Probe::CenterOn(ComboMenu::Probe::ArrowHead(node, slot));
        const POINT h = ComboMenu::Probe::ArrowHead(node, slot);
        const RECT target = ComboMenu::Probe::NodeRect(mod(node, slot));
        const COLORREF seen = pixel(h), want = ComboMenu::Probe::ArrowColor(node, slot);
        const RECT view = ComboMenu::Probe::Canvas();
        const bool ok = h.x >= 0 && !PtInRect(&target, h) && PtInRect(&view, h) && close_to(seen, want);
        if (!ok) std::printf("  head at %ld,%ld seen %06lx want %06lx\n", h.x, h.y, static_cast<unsigned long>(seen), static_cast<unsigned long>(want));
        return ok;
    };
    ComboMenu::Probe::Put(cut_node, free_node);
    for (const int percent : {50, 100, 150}) {
        ComboMenu::Probe::ZoomTo(percent); ComboMenu::Probe::Show(free_node);
        check(ComboMenu::Probe::Zoom() == percent && head_shows(free_node, NpcCombo::kModInterrupt),
              percent == 50 ? "the arrow to 允许中断 shows its head at 50 %" : percent == 100 ? "... at 100 %" : "... at 150 %");
        if (percent == 100) { std::wstring path = argv[1]; path.insert(path.size() - 4, L"_arrows"); shot(path); }
    }
    ComboMenu::Probe::ZoomTo(100); ComboMenu::Probe::Show(free_node);
    ComboMenu::Probe::CenterOn(ComboMenu::Probe::ArrowHead(free_node, NpcCombo::kModInterrupt));
    ComboMenu::Probe::MoveTo(weapon_node, ComboMenu::Probe::ArrowHead(free_node, NpcCombo::kModInterrupt));
    {
        const RECT over = ComboMenu::Probe::NodeRect(weapon_node); const POINT h = ComboMenu::Probe::ArrowHead(free_node, NpcCombo::kModInterrupt);
        check(PtInRect(&over, h) && head_shows(free_node, NpcCombo::kModInterrupt), "with another card laid over it, the head is drawn over the card");
        std::wstring path = argv[1]; path.insert(path.size() - 4, L"_covered"); shot(path);
    }
    // 投技: + 投技 after the selected move wires a grab node after it and opens its grab list at once: every supported
    // grab, this look's boss first, never the 投技 page's pick. Until a grab is picked the node is passed over; it takes
    // no arrows to modifiers.
    ComboMenu::Probe::Show(free_node); click(ComboMenu::Probe::Header(free_node));
    const int was_next = now().chain[static_cast<size_t>(free_node)].next;
    click(ComboMenu::Probe::Tool(12));
    const int grab_node = static_cast<int>(now().chain.size()) - 1;
    {
        // What the list should hold: the supported grabs, this boss's under one header, the others under one per boss.
        size_t supported = 0, boss_grabs = 0; std::set<std::wstring> other_bosses; std::wstring first, first_other;
        for (const MotianGrab::Variant& v : StubGrabs()) {
            if (!v.supported) continue;
            ++supported;
            if (GrabBoss::Same(v.boss_id, moves.profile)) { if (!boss_grabs) first = v.id; ++boss_grabs; }
            else { if (other_bosses.empty() || v.boss_id < *other_bosses.begin()) first_other = v.id; other_bosses.insert(v.boss_id); }
        }
        if (first.empty()) first = first_other;
        const NpcCombo::Config c = now();
        check(c.chain.back().type == NpcCombo::NodeType::Grab && c.chain[static_cast<size_t>(free_node)].next == grab_node && c.chain.back().next == was_next &&
              ComboMenu::Probe::SearchOpen() && ComboMenu::Probe::SearchRows() == supported + (boss_grabs ? 1 : 0) + other_bosses.size(),
              "+ 投技 wires a grab node after the selected move and lists every supported grab, this boss's first");
        check(!NpcCombo::IsMove(c.chain.back()) && NpcCombo::EmptyMove(c.chain.back()), "until a grab is picked the node is passed over (no 投技 page's pick)");
        key_down(VK_RETURN);
        std::string id; for (const wchar_t ch : first) id += static_cast<char>(ch);
        const NpcCombo::Config picked = now();
        check(!ComboMenu::Probe::SearchOpen() && picked.chain[static_cast<size_t>(grab_node)].ability == id && NpcCombo::IsMove(picked.chain.back()) &&
              NpcCombo::TakesModifiers(picked.chain.back()) && NpcCombo::CancelOf(picked, grab_node) == 0 && NpcCombo::Encode(picked).find(L"," + first) != std::wstring::npos,
              "Enter picks the first grab: stored on the node (G:x,y,next,id); a grab has a modifier pin (允许中断 only) and no 派生点");
        ComboMenu::Probe::Show(grab_node);
        { std::wstring path = argv[1]; path.insert(path.size() - 4, L"_grab"); shot(path); }
        click(ComboMenu::Probe::Field(grab_node));
        check(ComboMenu::Probe::SearchOpen() && ComboMenu::Probe::SearchRows() == supported + (boss_grabs ? 1 : 0) + other_bosses.size(), "its field opens the same list again");
        key_down(VK_ESCAPE);
        check(now().chain[static_cast<size_t>(grab_node)].ability == id, "Esc keeps the grab picked");
        // 不变身: the checkbox under the grab; saved as an "h:" mark.
        click(ComboMenu::Probe::Toggle(grab_node));
        check(now().chain[static_cast<size_t>(grab_node)].own_look && NpcCombo::Encode(now()).find(L"|h:" + std::to_wstring(grab_node)) != std::wstring::npos,
              "不变身 ticked on the grab: kept as h:<node>");
        { std::wstring path = argv[1]; path.insert(path.size() - 4, L"_grabownlook"); shot(path); }
        click(ComboMenu::Probe::Toggle(grab_node));
        check(!now().chain[static_cast<size_t>(grab_node)].own_look && NpcCombo::Encode(now()).find(L"|h:") == std::wstring::npos, "不变身 off again");
        // 投技预览: the mouse resting on the grab card asks for its executor montage.
        send(WM_MOUSEMOVE, ComboMenu::Probe::Header(grab_node)); ComboMenu::Tick(); Sleep(450); ComboMenu::Tick(); ComboMenu::Tick();
        check(preview_shown_.find("_Montage") != std::string::npos, "投技预览: the grab card shows its executor montage");
        send(WM_MOUSEMOVE, {canvas.left + 4, canvas.bottom - 4}); ComboMenu::Tick();
    }
    // BOSS 技能: + BOSS 技能 wires a node after the selected move and opens the look list at once; a look picked
    // opens its moves; the node keeps both (X:look,GA,...) and takes a 派生点 like a move.
    ComboMenu::Probe::Show(grab_node); click(ComboMenu::Probe::Header(grab_node));
    click(ComboMenu::Probe::Tool(13));
    const int boss_node = static_cast<int>(now().chain.size()) - 1;
    check(now().chain.back().type == NpcCombo::NodeType::BossSkill && now().chain[static_cast<size_t>(grab_node)].next == boss_node &&
          ComboMenu::Probe::SearchOpen() && ComboMenu::Probe::SearchRows() == 1 + std::size(kNpcSkillSets) - [] { size_t none = 0; for (const NpcSkillSet& s : kNpcSkillSets) if (!s.skill_count) ++none; return none; }(),
          "+ BOSS 技能 wires a node after the selected one and lists every look with moves");
    key_down(VK_DOWN); key_down(VK_RETURN);
    check(!now().chain[static_cast<size_t>(boss_node)].boss.empty() && now().chain[static_cast<size_t>(boss_node)].ability.empty() && ComboMenu::Probe::SearchOpen(),
          "a look picked: its moves open next");
    key_down(VK_RETURN);
    {
        const NpcCombo::Node n = now().chain[static_cast<size_t>(boss_node)];
        check(!n.ability.empty() && NpcCombo::IsMove(n) && NpcCombo::TakesModifiers(n) && NpcCombo::Encode(now()).find(L"|X:" + std::wstring(n.boss.begin(), n.boss.end())) != std::wstring::npos,
              "a move picked: the node is a move of the chain (X:look,GA,...) that takes modifiers");
    }
    ComboMenu::Probe::Show(boss_node);
    click(ComboMenu::Probe::Plus(boss_node));
    check(now().chain[static_cast<size_t>(boss_node)].ms == NpcCombo::kCancelStep, "its 派生点 steps like a move's");
    // At 75 %: 加速 may go to the right of the card (when something sits under it), still in view.
    ComboMenu::Probe::ZoomTo(75); ComboMenu::Probe::Put(speed_node, boss_node); ComboMenu::Probe::Show(boss_node);
    drag(ComboMenu::Probe::ModOut(boss_node), ComboMenu::Probe::Header(speed_node));
    check(mod(boss_node, NpcCombo::kModSpeed) == speed_node, "an arrow from its round pin onto 加速: the boss move plays faster too");
    ComboMenu::Probe::ZoomTo(100);
    ComboMenu::Probe::Show(boss_node);
    click(ComboMenu::Probe::Field(boss_node));
    check(ComboMenu::Probe::SearchOpen(), "its look field opens the look list again");
    key_down(VK_ESCAPE);
    { std::wstring path = argv[1]; path.insert(path.size() - 4, L"_boss"); shot(path); }
    // BOSS 援护: + BOSS 援护 wires a node after the selected move and opens the look list; a look picked opens its moves;
    // the node is a move of the chain (a:look,GA,...) that takes modifiers and has no 派生点; removing it rejoins the wire.
    {
        const int before_next = now().chain[static_cast<size_t>(boss_node)].next;
        ComboMenu::Probe::Show(boss_node); click(ComboMenu::Probe::Header(boss_node));
        click(ComboMenu::Probe::Tool(32));
        const int help = static_cast<int>(now().chain.size()) - 1;
        check(now().chain.back().type == NpcCombo::NodeType::Assist && now().chain[static_cast<size_t>(boss_node)].next == help && ComboMenu::Probe::SearchOpen() &&
              ComboMenu::Probe::SearchRows() == 1 + std::size(kNpcSkillSets) - [] { size_t none = 0; for (const NpcSkillSet& s : kNpcSkillSets) if (!s.skill_count) ++none; return none; }(),
              "+ BOSS 援护 wires a node after the selected one and lists every look with moves");
        key_down(VK_DOWN); key_down(VK_RETURN);
        check(!now().chain[static_cast<size_t>(help)].boss.empty() && now().chain[static_cast<size_t>(help)].ability.empty() && ComboMenu::Probe::SearchOpen(), "援护: a look picked opens its moves");
        key_down(VK_RETURN);
        const NpcCombo::Node n = now().chain[static_cast<size_t>(help)];
        check(!n.ability.empty() && NpcCombo::IsMove(n) && NpcCombo::TakesModifiers(n) && NpcCombo::CancelOf(now(), help) == 0 &&
              NpcCombo::Encode(now()).find(L"|a:" + std::wstring(n.boss.begin(), n.boss.end()) + L",") != std::wstring::npos,
              "援护: a move picked, saved as a:look,GA; it takes modifiers and has no 派生点");
        ComboMenu::Probe::Show(help);
        click(ComboMenu::Probe::Field(help));
        check(ComboMenu::Probe::SearchOpen(), "援护's look field opens the look list again");
        key_down(VK_ESCAPE);
        { std::wstring path = argv[1]; path.insert(path.size() - 4, L"_assist"); shot(path); }
        click(ComboMenu::Probe::Header(help)); click(ComboMenu::Probe::Tool(5));
        check(now().chain.size() == static_cast<size_t>(help) && now().chain[static_cast<size_t>(boss_node)].next == before_next, "removing the 援护 rejoins the wire");
    }
    // 变身 / 变回主角: + 变身 wires a node after the selected move and lists every look at once (looks without moves
    // too); the look picked is kept (|M:look,...). + 变回主角 wires a node after it (|F:...). Neither takes modifiers.
    ComboMenu::Probe::Show(boss_node); click(ComboMenu::Probe::Header(boss_node));
    click(ComboMenu::Probe::Tool(22));
    const int look_node = static_cast<int>(now().chain.size()) - 1;
    check(now().chain.back().type == NpcCombo::NodeType::Look && now().chain[static_cast<size_t>(boss_node)].next == look_node &&
          ComboMenu::Probe::SearchOpen() && ComboMenu::Probe::SearchRows() == 1 + std::size(kNpcSkillSets),
          "+ 变身 wires a node after the selected one and lists every look");
    key_down(VK_DOWN); key_down(VK_RETURN);
    {
        const NpcCombo::Node n = now().chain[static_cast<size_t>(look_node)];
        check(!n.boss.empty() && !ComboMenu::Probe::SearchOpen() && NpcCombo::IsMove(n) && !NpcCombo::TakesModifiers(n) &&
              NpcCombo::Encode(now()).find(L"|M:" + std::wstring(n.boss.begin(), n.boss.end()) + L",") != std::wstring::npos,
              "a look picked: the 变身 is a move of the chain (M:look,...) without modifiers");
    }
    ComboMenu::Probe::Show(look_node);
    click(ComboMenu::Probe::Field(look_node));
    check(ComboMenu::Probe::SearchOpen(), "its look field opens the look list again");
    key_down(VK_ESCAPE);
    click(ComboMenu::Probe::Header(look_node));
    click(ComboMenu::Probe::Tool(23));
    const int hero_node = static_cast<int>(now().chain.size()) - 1;
    check(now().chain.back().type == NpcCombo::NodeType::HeroLook && now().chain[static_cast<size_t>(look_node)].next == hero_node && !ComboMenu::Probe::SearchOpen() &&
          NpcCombo::Encode(now()).find(L"|F:") != std::wstring::npos && NpcCombo::Decode(NpcCombo::Encode(now())).chain[static_cast<size_t>(hero_node)].type == NpcCombo::NodeType::HeroLook,
          "+ 变回主角 wires a node after the 变身; the page saves both and reads them back");
    // 保留变身: + 保留变身 with the BOSS 技能 selected points it at a new 保留变身 (|E:...); the 变身 cannot take one.
    ComboMenu::Probe::Show(boss_node); click(ComboMenu::Probe::Header(boss_node));
    click(ComboMenu::Probe::Tool(24));
    const int keep_node = static_cast<int>(now().chain.size()) - 1;
    check(now().chain.back().type == NpcCombo::NodeType::KeepLook && mod(boss_node, NpcCombo::kModKeepLook) == keep_node && NpcCombo::KeepLookFor(now(), -1, boss_node) &&
          NpcCombo::Encode(now()).find(L"|E:") != std::wstring::npos && NpcCombo::KeepLookFor(NpcCombo::Decode(NpcCombo::Encode(now())), -1, boss_node),
          "+ 保留变身 with the BOSS 技能 selected: its look is kept (E:...), saved and read back");
    check(!NpcCombo::ModifierFits(now().chain, look_node, keep_node) && !NpcCombo::ModifierFits(now().chain, hero_node, keep_node), "a 变身 / 变回主角 cannot point at it");
    // 强制受击: + 强制受击 with the BOSS 技能 selected points it at a new one (击飞); its field lists the six kinds.
    ComboMenu::Probe::Show(boss_node); click(ComboMenu::Probe::Header(boss_node));
    click(ComboMenu::Probe::Tool(25));
    const int force_node = static_cast<int>(now().chain.size()) - 1;
    check(now().chain.back().type == NpcCombo::NodeType::ForceHit && mod(boss_node, NpcCombo::kModForceHit) == force_node &&
          NpcCombo::ForceHitFor(now(), -1, boss_node) == static_cast<int>(NpcCombo::HitKind::Launch), "+ 强制受击 with the BOSS 技能 selected: its hits force 击飞");
    ComboMenu::Probe::Show(force_node);
    click(ComboMenu::Probe::Field(force_node));
    check(ComboMenu::Probe::SearchOpen() && ComboMenu::Probe::SearchRows() == 7, "its field lists 击飞, 击倒, 挑空, 击退, 破防, 硬直");
    type(english ? L"guard" : L"破防"); key_down(VK_RETURN);
    check(now().chain[static_cast<size_t>(force_node)].hit == static_cast<int>(NpcCombo::HitKind::Break) && NpcCombo::Encode(now()).find(L"|Q:4,") != std::wstring::npos &&
          NpcCombo::ForceHitFor(NpcCombo::Decode(NpcCombo::Encode(now())), -1, boss_node) == static_cast<int>(NpcCombo::HitKind::Break), "破防 picked: saved as Q:4 and read back");
    { std::wstring path = argv[1]; path.insert(path.size() - 4, L"_forcehit"); shot(path); }
    { std::wstring path = argv[1]; path.insert(path.size() - 4, L"_look"); shot(path); }
    // 武器派生: + 武器派生 wires a node after the selected move and lists the weapons; a weapon picked opens its
    // abilities; a weapon row dropped on the node swaps its weapon, one dropped on bare canvas is a new node.
    ComboMenu::Probe::Show(boss_node); click(ComboMenu::Probe::Header(boss_node));
    click(ComboMenu::Probe::Tool(17));
    const int wm_node = static_cast<int>(now().chain.size()) - 1;
    check(now().chain.back().type == NpcCombo::NodeType::WeaponMove && now().chain[static_cast<size_t>(boss_node)].next == wm_node &&
          ComboMenu::Probe::SearchOpen() && ComboMenu::Probe::SearchRows() > 20, "+ 武器派生 wires a node after the selected one and lists the weapons");
    key_down(VK_DOWN); key_down(VK_RETURN);
    check(ComboWeapons::Find(now().chain[static_cast<size_t>(wm_node)].weapon) && now().chain[static_cast<size_t>(wm_node)].ability.empty() && ComboMenu::Probe::SearchOpen(),
          "a weapon picked: its abilities open next");
    key_down(VK_RETURN);
    {
        const NpcCombo::Node n = now().chain[static_cast<size_t>(wm_node)];
        check(ComboWeapons::MoveOf(n.weapon, n.ability) && NpcCombo::IsMove(n) && NpcCombo::TakesModifiers(n) &&
              NpcCombo::Encode(now()).find(L"|Y:" + std::wstring(n.weapon.begin(), n.weapon.end()) + L"," + std::wstring(n.ability.begin(), n.ability.end())) != std::wstring::npos,
              "an ability picked: the node is a move of the chain (Y:weapon,GA,...) that takes modifiers");
    }
    { const RECT c = ComboMenu::Probe::Canvas(); std::printf("weapon list rows=%zu canvas %ld..%ld\n", ComboMenu::Probe::WeaponRows(), c.left, c.right); }
    if (ComboMenu::Probe::WeaponRows()) {
        check(ComboMenu::Probe::WeaponRows() > 20 && ComboMenu::Probe::WeaponRowOf("Tang").x >= 0 && ComboMenu::Probe::WeaponRowOf("TangPrologue").x < 0,
              "the weapon list beside the canvas lists the weapons once per name");
        ComboMenu::Probe::Show(wm_node);
        drag(ComboMenu::Probe::WeaponRowOf("Tang"), ComboMenu::Probe::Header(wm_node));
        check(now().chain[static_cast<size_t>(wm_node)].weapon == "Tang" && ComboMenu::Probe::SearchOpen(), "a weapon dropped on the node swaps its weapon and opens its abilities");
        key_down(VK_RETURN);
        {
            const NpcCombo::Node n = now().chain[static_cast<size_t>(wm_node)];
            const ComboWeapons::Move* m = ComboWeapons::MoveOf(n.weapon, n.ability);
            check(m && m->kind == ComboWeapons::kLight && std::string(m->stage) == "A", "its first ability is the light combo's first stage (A)");
        }
        const size_t before_drop = now().chain.size();
        empty = bare();
        drag(ComboMenu::Probe::WeaponRowOf("IronBlade"), empty);
        check(now().chain.size() == before_drop + 1 && now().chain.back().type == NpcCombo::NodeType::WeaponMove && now().chain.back().weapon == "IronBlade" &&
              now().chain.back().next == -1 && ComboMenu::Probe::SearchOpen(), "a weapon dropped on bare canvas is a new free 武器派生 node, its abilities open");
        key_down(VK_ESCAPE);
    }
    // 伤害倍率: a modifier the selected move points at, 200% new, + steps 10%, typed percent held to 0..1000%.
    ComboMenu::Probe::Show(wm_node); click(ComboMenu::Probe::Header(wm_node));
    click(ComboMenu::Probe::Tool(18));
    const int damage_node = static_cast<int>(now().chain.size()) - 1;
    check(now().chain.back().type == NpcCombo::NodeType::Damage && mod(wm_node, NpcCombo::kModDamage) == damage_node && now().chain.back().speed == NpcCombo::kDamageNew,
          "+ 伤害倍率 with a move selected: the move points at it (200%)");
    ComboMenu::Probe::Show(damage_node);
    click(ComboMenu::Probe::Plus(damage_node));
    check(now().chain[static_cast<size_t>(damage_node)].speed == NpcCombo::kDamageNew + NpcCombo::kDamageStep, "its + adds 10%");
    click(ComboMenu::Probe::Value(damage_node)); type(L"350"); key_down(VK_RETURN);
    check(now().chain[static_cast<size_t>(damage_node)].speed == 350, "a typed 350 sets 350%");
    click(ComboMenu::Probe::Value(damage_node)); type(L"1500"); key_down(VK_RETURN);
    check(now().chain[static_cast<size_t>(damage_node)].speed == NpcCombo::kDamageMax && NpcCombo::kDamageMax == 1000, "a typed 1500 is held to 1000%");
    click(ComboMenu::Probe::Value(damage_node)); type(L"50"); key_down(VK_RETURN);
    check(now().chain[static_cast<size_t>(damage_node)].speed == 50 && NpcCombo::Encode(now()).find(L"|D:50,") != std::wstring::npos, "a typed 50 sets 50% and is saved (D:percent,...)");
    // 主角处决: a hero trigger whose weapon field picks whose executions it replaces (any weapon first).
    click(ComboMenu::Probe::Tool(2));
    const int exec_node = static_cast<int>(now().chain.size()) - 1;
    ComboMenu::Probe::Show(exec_node);
    click(ComboMenu::Probe::Field(exec_node));
    type(english ? L"execution" : L"处决"); key_down(VK_RETURN);
    check(now().chain[static_cast<size_t>(exec_node)].trigger == static_cast<int>(NpcCombo::Trigger::Execution), "the trigger list offers 主角处决");
    {
        const RECT card = ComboMenu::Probe::NodeRect(exec_node); const POINT f = ComboMenu::Probe::Field2(exec_node);
        check(PtInRect(&card, f) != FALSE && f.y > ComboMenu::Probe::Field(exec_node).y, "a 处决 trigger has a weapon row under its kind");
        click(f);
    }
    check(ComboMenu::Probe::SearchOpen() && ComboMenu::Probe::SearchRows() > 5, "its weapon field lists 任意武器 and the weapons with executions");
    key_down(VK_DOWN); key_down(VK_RETURN);
    {
        const std::string picked = now().chain[static_cast<size_t>(exec_node)].weapon;
        const ComboWeapons::Weapon* w = ComboWeapons::Find(picked);
        bool executes = false;
        if (w) for (size_t i = 0; i < w->count; ++i) executes = executes || w->moves[i].kind == ComboWeapons::kExecution;
        check(executes, "Down + Enter picks a weapon with executions; kept through save and load");
    }
    if (ComboMenu::Probe::WeaponRows()) {
        ComboMenu::Probe::Show(exec_node);
        drag(ComboMenu::Probe::WeaponRowOf("Tang"), ComboMenu::Probe::Header(exec_node));
        check(now().chain[static_cast<size_t>(exec_node)].weapon == "Tang", "a weapon dropped on a 处决 trigger sets whose executions it replaces");
    }
    // 自定义组合键: a hero trigger whose keys are picked one after another (the list stays open), pressed in order with
    // a gap the user sets, or together.
    click(ComboMenu::Probe::Tool(2));
    const int keys_node = static_cast<int>(now().chain.size()) - 1;
    ComboMenu::Probe::Show(keys_node);
    click(ComboMenu::Probe::Field(keys_node));
    type(english ? L"custom key" : L"组合键"); key_down(VK_RETURN);
    check(now().chain[static_cast<size_t>(keys_node)].trigger == static_cast<int>(NpcCombo::Trigger::Combo) && ComboMenu::Probe::SearchOpen() &&
          ComboMenu::Probe::SearchRows() == 10, "the trigger list offers 自定义组合键; picking it opens its keys at once (7 keys and 完成)");
    key_down(VK_RETURN);
    check(ComboMenu::Probe::SearchOpen() && now().chain[static_cast<size_t>(keys_node)].keys == std::vector<int>{static_cast<int>(NpcCombo::Trigger::Light)},
          "Enter adds 轻攻击 (the list starts on it) and the list stays open for the next key");
    key_down(VK_DOWN); key_down(VK_RETURN);
    check(now().chain[static_cast<size_t>(keys_node)].keys == std::vector<int>{static_cast<int>(NpcCombo::Trigger::Light), static_cast<int>(NpcCombo::Trigger::Heavy)},
          "Down + Enter adds 重攻击 after it");
    key_down(VK_ESCAPE);
    {
        const RECT card = ComboMenu::Probe::NodeRect(keys_node);
        const POINT f2 = ComboMenu::Probe::Field2(keys_node), f3 = ComboMenu::Probe::Field3(keys_node);
        check(PtInRect(&card, f2) != FALSE && PtInRect(&card, f3) != FALSE && f3.y > f2.y && f2.y > ComboMenu::Probe::Field(keys_node).y,
              "a 组合键 trigger has its keys and its pressing rows under its kind");
        ComboMenu::Probe::Show(keys_node);
        { std::wstring path = argv[1]; path.insert(path.size() - 4, L"_keys"); shot(path); }
        click(f3);
    }
    check(ComboMenu::Probe::SearchOpen() && ComboMenu::Probe::SearchRows() == 10, "its pressing field lists 一起按 and seven gaps in order");
    type(L"650"); key_down(VK_RETURN);
    check(!now().chain[static_cast<size_t>(keys_node)].chord && now().chain[static_cast<size_t>(keys_node)].window == 650 &&
          NpcCombo::Encode(now()).find(L",1;2/0/650") != std::wstring::npos, "a typed 650: in order, at most 650 ms between two keys (saved as keys/0/ms)");
    click(ComboMenu::Probe::Field3(keys_node)); type(L"5000"); key_down(VK_RETURN);
    check(now().chain[static_cast<size_t>(keys_node)].window == NpcCombo::kKeyWindowMax, "a typed 5000 is held to 2000 ms");
    click(ComboMenu::Probe::Field3(keys_node)); type(english ? L"together" : L"一起"); key_down(VK_RETURN);
    check(now().chain[static_cast<size_t>(keys_node)].chord && NpcCombo::Encode(now()).find(L",1;2/1/") != std::wstring::npos, "一起按 is saved (keys/1/ms)");
    click(ComboMenu::Probe::Field2(keys_node));
    check(ComboMenu::Probe::SearchOpen() && ComboMenu::Probe::SearchRows() == 12, "with keys, its keys' list also offers 删除最后一个按键 and 清空按键");
    type(english ? L"remove the last" : L"删除最后"); key_down(VK_RETURN);
    check(now().chain[static_cast<size_t>(keys_node)].keys.size() == 1 && ComboMenu::Probe::SearchOpen(), "删除最后一个按键 takes 重攻击 off; the list stays open");
    key_down(VK_ESCAPE);
    // 继续派生: the combo again by default; one of the hero's buttons (or the NPC key) instead.
    {
        const POINT f4 = ComboMenu::Probe::Field4(keys_node); const RECT kcard = ComboMenu::Probe::NodeRect(keys_node);
        check(f4.y > ComboMenu::Probe::Field3(keys_node).y && PtInRect(&kcard, f4) != FALSE && now().chain[static_cast<size_t>(keys_node)].then == NpcCombo::kThenOwn,
              "a 组合键 trigger has a 继续派生 field under its pressing field (default: the combo again)");
        click(f4);
        check(ComboMenu::Probe::SearchOpen() && ComboMenu::Probe::SearchRows() == 17, "its list: the combo again, then the NPC key and 13 hero buttons (no execution, no own keys)");
        type(english ? L"heavy attack" : L"重攻击"); key_down(VK_RETURN);
        check(now().chain[static_cast<size_t>(keys_node)].then == static_cast<int>(NpcCombo::Trigger::Heavy) && NpcCombo::Encode(now()).find(L",1/1/2000/2") != std::wstring::npos &&
              NpcCombo::Decode(NpcCombo::Encode(now())).chain[static_cast<size_t>(keys_node)].then == static_cast<int>(NpcCombo::Trigger::Heavy),
              "主角重攻击 picked: saved after the keys' mode (/2) and read back");
        ComboMenu::Probe::Show(keys_node);
        { std::wstring path = argv[1]; path.insert(path.size() - 4, L"_then"); shot(path); }
        click(ComboMenu::Probe::Field4(keys_node)); type(english ? L"the combo again" : L"再按原组合键"); key_down(VK_RETURN);
        check(now().chain[static_cast<size_t>(keys_node)].then == NpcCombo::kThenOwn && NpcCombo::Encode(now()).find(L",1/1/2000/2") == std::wstring::npos,
              "再按原组合键 puts the default back (nothing saved)");
        // 派生提示: the header switch for the notice in the game's bottom-right corner.
        const bool was = NpcCombo::ShowToast();
        click(ComboMenu::Probe::Tool(27));
        const bool flipped = NpcCombo::ShowToast() == !was;
        click(ComboMenu::Probe::Tool(27));
        check(flipped && NpcCombo::ShowToast() == was, "the 派生提示 switch turns the chain's notice off and on again");
        // 测试木桩: the header button asks for the mob; once it stands the button takes it away again.
        const int asked = dummy_requests_;
        click(ComboMenu::Probe::Tool(28));
        const bool spawned = dummy_requests_ == asked + 1 && dummy_standing_;
        for (int i = 0; i < 8; ++i) { ComboMenu::Tick(); Sleep(30); }
        click(ComboMenu::Probe::Tool(28));
        check(spawned && dummy_requests_ == asked + 2 && !dummy_standing_, "the 测试木桩 button spawns the test mob, then removes it");
        for (int i = 0; i < 8; ++i) { ComboMenu::Tick(); Sleep(30); }
    }
    // 自定义按键: keys recorded from the keyboard and a pad, or picked from the pad's buttons; one key, in order or
    // together; 吞掉 off by default.
    click(ComboMenu::Probe::Tool(2));
    const int raw_node = static_cast<int>(now().chain.size()) - 1;
    ComboMenu::Probe::Show(raw_node);
    click(ComboMenu::Probe::Field(raw_node));
    type(english ? L"custom keys" : L"自定义按键"); key_down(VK_RETURN);
    check(now().chain[static_cast<size_t>(raw_node)].trigger == static_cast<int>(NpcCombo::Trigger::Keys) && ComboMenu::Probe::SearchOpen() &&
          ComboMenu::Probe::SearchRows() == 25 && !now().chain[static_cast<size_t>(raw_node)].swallow,
          "the trigger list offers 自定义按键; its keys' list opens at once (录制, 16 pad buttons, 4 left-stick ways, 完成); 吞掉 is off");
    key_down(VK_RETURN);
    check(ComboMenu::Probe::RecordingNode() == raw_node && !ComboMenu::Probe::SearchOpen(), "the list starts on 录制: Enter starts recording");
    key_down('Q');
    UiHotkeys::g_pad = 1u << 8; ComboMenu::Tick(); UiHotkeys::g_pad = 0; ComboMenu::Tick();
    check(ComboMenu::Probe::RecordingKeys() == std::vector<int>{'Q', NpcCombo::kPadKey + 8}, "a key and pad button join the unsaved recording draft");
    { std::wstring path = argv[1]; path.insert(path.size() - 4, L"_record"); shot(path); }
    key_down(VK_RETURN);
    check(ComboMenu::Probe::RecordingNode() < 0 && NpcCombo::Encode(now()).find(L",81;264/0/400/0") != std::wstring::npos,
          "Enter confirms recording; saved as keys/0/window/0 (in order, not swallowed)");
    click(ComboMenu::Probe::Toggle2(raw_node));
    check(now().chain[static_cast<size_t>(raw_node)].swallow && NpcCombo::Encode(now()).find(L",81;264/0/400/1") != std::wstring::npos, "its 吞掉 box swallows the keys' game actions (saved /1)");
    click(ComboMenu::Probe::Field3(raw_node)); type(english ? L"together" : L"一起"); key_down(VK_RETURN);
    check(now().chain[static_cast<size_t>(raw_node)].chord, "its pressing field: 一起按");
    click(ComboMenu::Probe::Field4(raw_node));
    check(ComboMenu::Probe::SearchOpen() && ComboMenu::Probe::SearchRows() == 40, "its 继续派生 list: its keys again, 录制, 20 pad keys and the hero's buttons");
    type(english ? L"record one key" : L"录制一个键"); key_down(VK_RETURN);
    check(ComboMenu::Probe::RecordingNode() == raw_node && ComboMenu::Probe::RecordingRoute() == -5 && !ComboMenu::Probe::SearchOpen(), "录制一个键 records its 继续派生 key");
    key_down('E'); key_down(VK_RETURN);
    check(ComboMenu::Probe::RecordingNode() < 0 && now().chain[static_cast<size_t>(raw_node)].then == NpcCombo::kThenRaw + 'E' &&
          now().chain[static_cast<size_t>(raw_node)].keys == std::vector<int>{'Q', NpcCombo::kPadKey + 8} && NpcCombo::Encode(now()).find(L",81;264/1/400/1/4165") != std::wstring::npos,
          "one key pressed: it is the 继续派生 key (the trigger's own keys stay), saved after 吞掉 (/4165) and the recording ends");
    click(ComboMenu::Probe::Field2(raw_node));
    check(ComboMenu::Probe::SearchOpen() && ComboMenu::Probe::SearchRows() == 27, "with keys, the list also offers 删除最后一个按键 and 清空按键");
    type(L"RB"); key_down(VK_RETURN);
    check(now().chain[static_cast<size_t>(raw_node)].keys.size() == 3 && now().chain[static_cast<size_t>(raw_node)].keys.back() == NpcCombo::kPadKey + 9 &&
          ComboMenu::Probe::SearchOpen(), "a pad button picked by name (RB / R1) joins it; the list stays open");
    type(english ? L"ls down" : L"左摇杆下推"); key_down(VK_RETURN);
    check(now().chain[static_cast<size_t>(raw_node)].keys.size() == 4 && now().chain[static_cast<size_t>(raw_node)].keys.back() == NpcCombo::kStickKey + 1,
          "a left-stick way picked by name (左摇杆下推) joins it");
    key_down(VK_ESCAPE);
    {
        // Recording a motion input: 上推, 下推 (the stick through the middle), then the pad's X / 方块.
        click(ComboMenu::Probe::Field2(raw_node)); type(english ? L"clear" : L"清空按键"); key_down(VK_RETURN); key_down(VK_ESCAPE);
        click(ComboMenu::Probe::Field2(raw_node)); key_down(VK_RETURN);
        const bool recording = ComboMenu::Probe::RecordingNode() == raw_node;
        UiHotkeys::g_stick = 1; ComboMenu::Tick(); UiHotkeys::g_stick = 0; ComboMenu::Tick();
        UiHotkeys::g_stick = 2; ComboMenu::Tick(); UiHotkeys::g_stick = 0; ComboMenu::Tick();
        UiHotkeys::g_pad = 1u << 12; ComboMenu::Tick(); UiHotkeys::g_pad = 0; ComboMenu::Tick();
        key_down(VK_RETURN);
        check(recording && now().chain[static_cast<size_t>(raw_node)].keys == std::vector<int>{NpcCombo::kStickKey, NpcCombo::kStickKey + 1, NpcCombo::kPadKey + 12} &&
              NpcCombo::Encode(now()).find(L",272;273;268/") != std::wstring::npos,
              "recording takes the left stick's ways as keys: 上推, 下推, 方块 (saved 272;273;268)");
        ComboMenu::Probe::Show(raw_node);
        { std::wstring path = argv[1]; path.insert(path.size() - 4, L"_motion"); shot(path); }
        // Six keys of every kind: shown as icons, the card grows to fit them all.
        const LONG three = ComboMenu::Probe::NodeRect(raw_node).right - ComboMenu::Probe::NodeRect(raw_node).left;
        click(ComboMenu::Probe::Field2(raw_node));
        for (const wchar_t* name : {L"LB", english ? L"ls left" : L"左摇杆左推", L"RT"}) { type(name); key_down(VK_RETURN); }
        key_down(VK_ESCAPE);
        const LONG six = ComboMenu::Probe::NodeRect(raw_node).right - ComboMenu::Probe::NodeRect(raw_node).left;
        check(now().chain[static_cast<size_t>(raw_node)].keys.size() == 6 && six > three,
              "keys show as icons: six of them widen the card so every icon fits (no ellipsis)");
        ComboMenu::Probe::Show(raw_node);
        { std::wstring path = argv[1]; path.insert(path.size() - 4, L"_icons"); shot(path); }
    }
    ComboMenu::Probe::Show(raw_node);
    { std::wstring path = argv[1]; path.insert(path.size() - 4, L"_rawkeys"); shot(path); }
    ComboMenu::Probe::Show(wm_node);
    { std::wstring path = argv[1]; path.insert(path.size() - 4, L"_weapon"); shot(path); }
    // 武器派生修改: + 武器派生修改 adds one below everything and opens its weapon list; its rows (完美闪避 / 处决 / 简易处决
    // / 格挡 new) are outputs of their own, each one's action picked from the hero's actions; it starts chains, so no
    // wire goes into it.
    {
        const size_t wa_before = now().chain.size();
        click(ComboMenu::Probe::Tool(26));
        const int wa_node = static_cast<int>(now().chain.size()) - 1;
        check(now().chain.size() == wa_before + 1 && now().chain.back().type == NpcCombo::NodeType::WeaponActions && now().chain.back().routes.size() == 4 &&
              ComboMenu::Probe::SearchOpen() && ComboMenu::Probe::SearchRows() > 5, "+ 武器派生修改 adds one with four rows and opens its weapon list");
        key_down(VK_RETURN);
        check(!now().chain[static_cast<size_t>(wa_node)].weapon.empty() && !ComboMenu::Probe::SearchOpen(), "Enter picks its weapon");
        ComboMenu::Probe::Put(wa_node, wm_node);
        ComboMenu::Probe::ZoomTo(50);  // the move and the whole card (its row pins on the right) in view
        ComboMenu::Probe::Show(wm_node);
        click(ComboMenu::Probe::RouteField(wa_node, 3));
        check(ComboMenu::Probe::SearchOpen() && ComboMenu::Probe::SearchRows() == 16, "a row's action lists the hero's actions (15: no NPC key, no own keys)");
        type(english ? L"deflect" : L"偏斜"); key_down(VK_RETURN);
        check(now().chain[static_cast<size_t>(wa_node)].routes[3].button == static_cast<int>(NpcCombo::Trigger::DeflectDash), "picking 主角偏斜闪避 sets the row's action");
        drag(ComboMenu::Probe::RouteOut(wa_node, 0), ComboMenu::Probe::Pin(wm_node, false));
        check(now().chain[static_cast<size_t>(wa_node)].routes[0].next == wm_node && now().chain[static_cast<size_t>(wa_node)].next == -1,
              "a row's pin wires that action to a move");
        drag(ComboMenu::Probe::Pin(wm_node, true), ComboMenu::Probe::Header(wa_node));
        check(now().chain[static_cast<size_t>(wm_node)].next != wa_node, "no wire goes into it");
        click(ComboMenu::Probe::RouteAdd(wa_node));
        check(now().chain[static_cast<size_t>(wa_node)].routes.size() == 5 &&
              now().chain[static_cast<size_t>(wa_node)].routes[4].button == static_cast<int>(NpcCombo::Trigger::Stance), "+ 添加动作 adds the first usual action no row has (格挡)");
        check(NpcCombo::Encode(now()).find(L"|O:") != std::wstring::npos && NpcCombo::Encode(NpcCombo::Decode(NpcCombo::Encode(now()))) == NpcCombo::Encode(now()),
              "saved as O:... and read back the same");
        ComboMenu::Probe::ZoomTo(100);
        ComboMenu::Probe::Show(wa_node);
        { std::wstring path = argv[1]; path.insert(path.size() - 4, L"_actions"); shot(path); }
        // 框选: Ctrl + drag from bare canvas selects every card the box touches; dragging one selected card moves them
        // all; a click on bare canvas drops the selection; Ctrl + click adds a node; Delete removes the whole selection.
        ComboMenu::Probe::ZoomTo(50);
        ComboMenu::Probe::Show(wm_node);
        const auto empty_near = [&](POINT p) {
            for (int k = 0; k < 40 && !ComboMenu::Probe::Empty(p); ++k) { p.x += 9; p.y += (k % 2) ? 7 : 0; }
            return p;
        };
        const auto box = [&](POINT p, POINT q) {
            send(WM_LBUTTONDOWN, p, MK_LBUTTON | MK_CONTROL);
            for (int i = 1; i <= 8; ++i) send(WM_MOUSEMOVE, {p.x + (q.x - p.x) * i / 8, p.y + (q.y - p.y) * i / 8}, MK_LBUTTON | MK_CONTROL);
            send(WM_LBUTTONUP, q);
        };
        {
            const RECT a = ComboMenu::Probe::NodeRect(wm_node), b = ComboMenu::Probe::NodeRect(wa_node);
            const POINT from = empty_near({(std::max)(a.right, b.right) + 12, (std::max)(a.bottom, b.bottom) + 12});
            const POINT to{(std::min)(a.left, b.left) - 6, (std::min)(a.top, b.top) - 6};
            const bool from_bare = ComboMenu::Probe::Empty(from);
            box(from, to);
            check(from_bare && ComboMenu::Probe::PickedNode(wm_node) && ComboMenu::Probe::PickedNode(wa_node) && ComboMenu::Probe::Picked() >= 2,
                  "Ctrl + drag from bare canvas box-selects the cards it touches");
            const NpcCombo::Node m0 = now().chain[static_cast<size_t>(wm_node)], w0 = now().chain[static_cast<size_t>(wa_node)];
            const POINT h = ComboMenu::Probe::Header(wm_node);
            drag(h, {h.x + 40, h.y + 30});
            const NpcCombo::Node m1 = now().chain[static_cast<size_t>(wm_node)], w1 = now().chain[static_cast<size_t>(wa_node)];
            check(m1.x != m0.x && m1.x - m0.x == w1.x - w0.x && m1.y - m0.y == w1.y - w0.y && ComboMenu::Probe::Picked() >= 2,
                  "dragging one selected card moves the whole selection");
            click(empty_near({ComboMenu::Probe::Canvas().left + 30, ComboMenu::Probe::Canvas().bottom - 40}));
            check(ComboMenu::Probe::Picked() == 0, "a click on bare canvas drops the selection");
        }
        {
            click(ComboMenu::Probe::Tool(1));  // an interval after the key's chain
            const int iv = static_cast<int>(now().chain.size()) - 1;
            ComboMenu::Probe::Put(iv, wa_node); ComboMenu::Probe::Show(wa_node);
            click(ComboMenu::Probe::Header(wa_node));
            const POINT ih = ComboMenu::Probe::Header(iv);
            send(WM_LBUTTONDOWN, ih, MK_LBUTTON | MK_CONTROL); send(WM_LBUTTONUP, ih);
            check(ComboMenu::Probe::Picked() == 2 && ComboMenu::Probe::PickedNode(wa_node) && ComboMenu::Probe::PickedNode(iv), "Ctrl + click adds a node to the selection");
            const size_t before_delete = now().chain.size();
            key_down(VK_DELETE);
            bool any_actions = false;
            for (const NpcCombo::Node& n : now().chain) any_actions = any_actions || n.type == NpcCombo::NodeType::WeaponActions;
            check(now().chain.size() == before_delete - 2 && !any_actions && ComboMenu::Probe::Picked() == 0, "Delete removes the whole selection");
        }
        ComboMenu::Probe::ZoomTo(100);
    }
    // 组合 and 瞬移: + 组合 frames a group after the selected move, + 瞬移 a teleport after the group; moves dropped in the
    // frame join it (left to right, or along their own wires); wires never cross the frame; the header drags what
    // it holds; the corner resizes it; a teleport's distance / height fields list values or take a typed number.
    ComboMenu::Probe::Show(wm_node); click(ComboMenu::Probe::Header(wm_node));
    click(ComboMenu::Probe::Tool(19));
    const int group_node = static_cast<int>(now().chain.size()) - 1;
    check(now().chain.back().type == NpcCombo::NodeType::Group && now().chain[static_cast<size_t>(wm_node)].next == group_node &&
          now().chain.back().w == NpcCombo::kGroupNewW && NpcCombo::Encode(now()).find(L"|U:") != std::wstring::npos, "+ 组合 wires an empty frame after the selected move (U:...)");
    click(ComboMenu::Probe::Tool(20));
    const int tp_node = static_cast<int>(now().chain.size()) - 1;
    check(now().chain.back().type == NpcCombo::NodeType::Teleport && now().chain[static_cast<size_t>(group_node)].next == tp_node && now().chain.back().group == -1 &&
          now().chain.back().dist == NpcCombo::kTeleportDistDefault && now().chain.back().lift == NpcCombo::kTeleportLiftDefault, "+ 瞬移 with the frame selected goes after it, outside it (150 cm, 30 cm)");
    ComboMenu::Probe::Show(tp_node);
    click(ComboMenu::Probe::Field(tp_node));
    check(ComboMenu::Probe::SearchOpen() && ComboMenu::Probe::SearchRows() == 6, "its distance field lists five distances");
    type(L"250"); key_down(VK_RETURN);
    check(now().chain[static_cast<size_t>(tp_node)].dist == 250, "a typed 250 is a custom distance");
    click(ComboMenu::Probe::Field2(tp_node)); key_down(VK_DOWN); key_down(VK_RETURN);
    check(now().chain[static_cast<size_t>(tp_node)].lift == 60, "its height field starts on 30 cm; Down + Enter picks 60 cm");
    click(ComboMenu::Probe::Field2(tp_node)); type(L"999"); key_down(VK_RETURN);
    check(now().chain[static_cast<size_t>(tp_node)].lift == NpcCombo::kTeleportLiftMax && NpcCombo::Encode(now()).find(L"|P:250,300,") != std::wstring::npos,
          "a typed 999 cm height is held to 300 cm and saved (P:distance,height,...)");
    ComboMenu::Probe::ZoomTo(50);  // a frame is wide: all of it in view
    ComboMenu::Probe::Show(group_node);
    click(ComboMenu::Probe::Plus(group_node));
    check(now().chain[static_cast<size_t>(group_node)].ms == NpcCombo::kCancelStep, "the frame's own 派生点 steps like a move's");
    {
        const RECT frame = ComboMenu::Probe::NodeRect(group_node);
        const POINT at = ComboMenu::Probe::Body(group_node);
        drag(ComboMenu::Probe::Pill(0), at);
        drag(ComboMenu::Probe::Pill(1), {at.x, at.y + (frame.bottom - frame.top) * 2 / 5});  // under the first, clear of it
    }
    const int m1 = static_cast<int>(now().chain.size()) - 2, m2 = m1 + 1;
    check(now().chain[static_cast<size_t>(m1)].group == group_node && now().chain[static_cast<size_t>(m2)].group == group_node &&
          NpcCombo::GroupSteps(now(), group_node) == std::vector<int>{m1, m2}, "moves dropped in the frame join it, played left to right, then top to bottom");
    {
        const RECT frame = ComboMenu::Probe::NodeRect(group_node), card = ComboMenu::Probe::NodeRect(m2);
        check(card.right <= frame.right && card.bottom <= frame.bottom, "the frame grows to hold what is dropped in it");
    }
    drag(ComboMenu::Probe::Pin(m2, true), ComboMenu::Probe::Pin(m1, false));
    check(now().chain[static_cast<size_t>(m2)].next == m1 && NpcCombo::GroupSteps(now(), group_node) == std::vector<int>{m2, m1}, "a wire inside the frame sets the order");
    drag(ComboMenu::Probe::Pin(m1, true), ComboMenu::Probe::Pin(tp_node, false));
    check(now().chain[static_cast<size_t>(m1)].next == -1 && now().chain[static_cast<size_t>(m1)].group == group_node && now().chain[static_cast<size_t>(m2)].group == group_node,
          "a wire from a node in the frame to one outside is refused");
    click(ComboMenu::Probe::Header(m2));
    click(ComboMenu::Probe::Tool(20));
    const int tp_in = static_cast<int>(now().chain.size()) - 1;
    check(now().chain.back().group == group_node && now().chain[static_cast<size_t>(m2)].next == tp_in && now().chain.back().next == m1 &&
          NpcCombo::GroupSteps(now(), group_node) == std::vector<int>{m2, tp_in, m1}, "+ 瞬移 with a node of the frame selected goes into the frame after it");
    const int view_h = canvas.bottom - ComboMenu::Probe::FreeTop();  // the canvas under the tool bar (it wraps to six rows on a narrow English page)
    {
        // Out of the frame: its last node onto bare canvas right of it (the frame's right edge put mid-canvas, the
        // node a quarter below the middle, clear of the tool bar).
        ComboMenu::Probe::CenterOn({ComboMenu::Probe::NodeRect(group_node).right, ComboMenu::Probe::Header(tp_in).y - view_h / 4});
        const RECT frame = ComboMenu::Probe::NodeRect(group_node);
        POINT out{-1, -1};
        for (int y = canvas.bottom - 40; y > ComboMenu::Probe::FreeTop() + 20 && out.x < 0; y -= 20)
            for (int x = canvas.right - 40; x > canvas.left + 140 && out.x < 0; x -= 20) {
                bool ok = true;
                for (int dy = -20; dy <= 60 && ok; dy += 20) for (int dx = -120; dx <= 40 && ok; dx += 20) { const POINT q{x + dx, y + dy}; ok = ComboMenu::Probe::Empty(q) && !PtInRect(&frame, q); }
                if (ok) out = {x, y};
            }
        if (out.x >= 0) {
            drag(ComboMenu::Probe::Header(tp_in), out);
            check(now().chain[static_cast<size_t>(tp_in)].group == -1 && now().chain[static_cast<size_t>(m2)].next == -1 && now().chain[static_cast<size_t>(tp_in)].next == -1 &&
                  NpcCombo::GroupSteps(now(), group_node) == std::vector<int>{m1, m2}, "a node dragged out of the frame leaves it; the wires over the edge are cut");
        } else check(false, "bare canvas outside the frame");
    }
    {
        ComboMenu::Probe::Show(group_node);
        const NpcCombo::Node g0 = now().chain[static_cast<size_t>(group_node)], a0 = now().chain[static_cast<size_t>(m1)];
        const POINT h = ComboMenu::Probe::Header(group_node);
        drag(h, {h.x + 60, h.y + 40});
        const NpcCombo::Node g1 = now().chain[static_cast<size_t>(group_node)], a1 = now().chain[static_cast<size_t>(m1)];
        check(g1.x != g0.x && g1.x - g0.x == a1.x - a0.x && g1.y - g0.y == a1.y - a0.y && a1.group == group_node, "dragging the frame's header moves what it holds");
        const POINT corner0 = ComboMenu::Probe::Resize(group_node);
        ComboMenu::Probe::CenterOn({corner0.x, corner0.y - view_h / 4});  // the corner a quarter below the middle
        const POINT grip = ComboMenu::Probe::Resize(group_node);
        drag(grip, {grip.x + 80, grip.y + 60});
        check(now().chain[static_cast<size_t>(group_node)].w > g1.w && now().chain[static_cast<size_t>(group_node)].h > g1.h, "its corner makes it larger");
        ComboMenu::Probe::Show(group_node);
    }
    { std::wstring path = argv[1]; path.insert(path.size() - 4, L"_group"); shot(path); }
    {
        const NpcCombo::Config back = NpcCombo::Decode(NpcCombo::Encode(now()));
        check(NpcCombo::GroupSteps(back, group_node) == NpcCombo::GroupSteps(now(), group_node) && back.chain[static_cast<size_t>(tp_node)].lift == NpcCombo::kTeleportLiftMax,
              "the frame and the teleport read back as saved");
    }
    // Files: 文件 lists combos\*.combo; 另存为 opens Windows' own Save As window, whose typed name becomes the active
    // file; picking default goes back. The window is modal with its own loop on this thread: a thread timer finds it,
    // posts the name to its file name box as the keyboard would, then OK (it is closed after 5 s otherwise).
    click(ComboMenu::Probe::Tool(14));
    check(ComboMenu::Probe::SearchOpen() && ComboMenu::Probe::SearchRows() == 2 && NpcCombo::GetFiles().active == L"default", "文件 lists the one file (default.combo)");
    key_down(VK_ESCAPE);
    {
        static int ticks; static bool typed; ticks = 0; typed = false;
        const UINT_PTR timer = SetTimer(nullptr, 0, 30, [](HWND, UINT, UINT_PTR, DWORD) {
            ++ticks;
            HWND dialog = nullptr;
            EnumThreadWindows(GetCurrentThreadId(), [](HWND w, LPARAM out) -> BOOL {
                wchar_t type[32]{}; GetClassNameW(w, type, 32);
                if (IsWindowVisible(w) && !wcscmp(type, L"#32770")) { *reinterpret_cast<HWND*>(out) = w; return FALSE; }
                return TRUE; }, reinterpret_cast<LPARAM>(&dialog));
            if (!dialog || typed) return;
            HWND edit = nullptr;
            EnumChildWindows(dialog, [](HWND w, LPARAM out) -> BOOL {
                wchar_t type[32]{}, parent[32]{}; GetClassNameW(w, type, 32); GetClassNameW(GetParent(w), parent, 32);
                if (IsWindowVisible(w) && !wcscmp(type, L"Edit") && !wcscmp(parent, L"ComboBox")) { *reinterpret_cast<HWND*>(out) = w; return FALSE; }
                return TRUE; }, reinterpret_cast<LPARAM>(&edit));
            if (!edit) { if (ticks > 160) PostMessageW(dialog, WM_CLOSE, 0, 0); return; }
            typed = true;
            SendMessageW(edit, EM_SETSEL, 0, -1);
            for (const wchar_t* c = L"测试2"; *c; ++c) PostMessageW(edit, WM_CHAR, *c, 0);
            PostMessageW(dialog, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
        });
        click(ComboMenu::Probe::Tool(16));
        KillTimer(nullptr, timer);
        check(typed && NpcCombo::GetFiles().active == L"测试2" && NpcCombo::GetFiles().names.size() == 2 &&
              GetFileAttributesW((combos + L"\\测试2.combo").c_str()) != INVALID_FILE_ATTRIBUTES,
              "另存为 opens Windows' Save As window; the name typed there is written and becomes the active file");
    }
    const std::wstring boss_text = NpcCombo::Encode(now());
    click(ComboMenu::Probe::Tool(14));
    check(ComboMenu::Probe::SearchRows() == 4, "文件 lists both, the active one first");
    key_down(VK_DOWN); key_down(VK_RETURN);
    if (NpcCombo::Encode(now()) != boss_text) std::printf("before: %ls\nafter:  %ls\n", boss_text.c_str(), NpcCombo::Encode(now()).c_str());
    check(NpcCombo::GetFiles().active == L"default" && NpcCombo::Encode(now()) == boss_text, "picking default loads it (the same chains, saved before the copy)");
    // 按摇杆方向闪避: a tool button of its own (it was in the search list only): a look with dodges gets the node, one
    // without none.
    {
        const size_t nodes_before = now().chain.size();
        click(ComboMenu::Probe::Tool(29));
        if (moves.dodges.empty()) check(now().chain.size() == nodes_before, "按摇杆方向闪避 adds nothing for a look without dodges");
        else check(now().chain.size() == nodes_before + 1 && now().chain.back().type == NpcCombo::NodeType::AutoDodge, "the 按摇杆方向闪避 button adds a stick-dodge node");
    }
    // 派生表: the header lists the chains; the hero's or another look's is edited while the game keeps its look.
    {
        const std::string game = ComboMenu::Probe::EditedProfile();
        check(game == moves.profile, "the page edits the applied look's chain (跟随当前外观)");
        const size_t looks = std::size(kNpcSkillSets);
        size_t chained = 0;
        const auto all = NpcCombo::All();
        for (size_t i = 0; i < looks; ++i)
            for (const auto& [profile, cfg] : all)
                if (profile == kNpcSkillSets[i].profile && std::any_of(cfg.chain.begin(), cfg.chain.end(), [](const NpcCombo::Node& n) { return NpcCombo::IsMove(n); })) { ++chained; break; }
        click(ComboMenu::Probe::Header());
        check(ComboMenu::Probe::SearchOpen() && ComboMenu::Probe::SearchRows() == 3 + looks + (chained ? 1 : 0) + (chained < looks ? 1 : 0),
              "the header lists 跟随当前外观, 主角, the looks with a chain, then the others");
        key_down(VK_ESCAPE);
        const size_t hero_before = NpcCombo::Load(NpcCombo::kHeroProfile).chain.size(), game_before = now().chain.size();
        click(ComboMenu::Probe::Header()); type(english ? L"hero" : L"主角"); key_down(VK_RETURN);
        check(ComboMenu::Probe::EditedProfile() == NpcCombo::kHeroProfile && !ComboMenu::Probe::SearchOpen(), "主角 picked: the page edits the hero's chain (the game keeps its look)");
        click(ComboMenu::Probe::Tool(1));
        check(NpcCombo::Load(NpcCombo::kHeroProfile).chain.size() == hero_before + 1 && now().chain.size() == game_before, "a node added there goes into the hero's chain only");
        { std::wstring path = argv[1]; path.insert(path.size() - 4, L"_chains"); shot(path); }
        // Another look: one whose id no other id contains, typed.
        std::string other;
        for (size_t i = 0; i < looks && other.empty(); ++i) {
            const std::string id = kNpcSkillSets[i].profile;
            if (id == game || id.size() < 4) continue;
            std::string low = id; for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            bool unique = low.find("hero") == std::string::npos;
            for (size_t k = 0; k < looks && unique; ++k) {
                if (k == i) continue;
                std::string o = kNpcSkillSets[k].profile; for (char& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                unique = o.find(low) == std::string::npos;
            }
            if (unique) other = id;
        }
        check(!other.empty(), "a look to type");
        click(ComboMenu::Probe::Header()); type(std::wstring(other.begin(), other.end()).c_str()); key_down(VK_RETURN);
        check(ComboMenu::Probe::EditedProfile() == other && now().chain.size() == game_before, "another look's chain is edited without applying it");
        click(ComboMenu::Probe::Header()); type(english ? L"follow the current" : L"跟随当前"); key_down(VK_RETURN);
        check(ComboMenu::Probe::EditedProfile() == game, "跟随当前外观 goes back to the applied look's chain");
    }
    SendMessageW(page_, WM_KEYDOWN, VK_HOME, 0);
    { std::wstring path = argv[1]; path.insert(path.size() - 4, L"_after"); shot(path); }
    std::printf("checks failures=%d\n", failures);
    ComboMenu::Shutdown(); DestroyWindow(host_);
    return failures ? 1 : 0;
}
