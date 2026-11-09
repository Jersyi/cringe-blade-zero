#include "net_page.h"
#include "debug_hub.h"
#include "net_puppet.h"
#include "net_steam.h"
#include "ui_i18n.h"
#include "ui_layout.h"
#include "ui_theme.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace NetPage {
namespace {
constexpr wchar_t kClass[] = L"PBZNetPage_v1";
enum : int { kHost = 601, kOverlay, kLeave, kPing, kFriends, kInvite, kJoinFriend, kRefresh, kRoom, kJoin, kCopy, kSpawn, kRemove, kMirror };
HWND window_ = nullptr, title_ = nullptr, intro_ = nullptr, status_ = nullptr, room_ = nullptr, host_ = nullptr, overlay_ = nullptr, leave_ = nullptr,
     ping_ = nullptr, friends_label_ = nullptr, friends_ = nullptr, invite_ = nullptr, join_friend_ = nullptr, refresh_ = nullptr, room_label_ = nullptr,
     room_id_ = nullptr, join_ = nullptr, copy_ = nullptr, puppet_ = nullptr, spawn_ = nullptr, remove_ = nullptr, mirror_ = nullptr,
     puppet_status_ = nullptr, note_ = nullptr;
std::vector<uint64_t> friend_ids_;
uint64_t friends_serial_ = ~0ull, lobby_ = 0;
ULONGLONG next_refresh_ = 0;
UiI18n::Language language_ = UiI18n::Language::SimplifiedChinese;
std::wstring status_text_, puppet_text_;

std::wstring Tr(const wchar_t* text) { return UiI18n::Translate(text ? text : L""); }
// bind: the caption is a dictionary key (static text); live text is written with SetWindowTextW instead.
HWND Control(const wchar_t* cls, const wchar_t* text, DWORD style, int id = 0, bool bind = true) {
    const bool edit = !_wcsicmp(cls, L"EDIT") || !_wcsicmp(cls, L"LISTBOX");
    const HWND control = CreateWindowExW(edit ? WS_EX_CLIENTEDGE : 0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    if (control) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(UiTheme::GetFont(UiTheme::Font::Body, window_)), FALSE);
        if (bind) UiI18n::Bind(control, text);
    }
    return control;
}
bool English() { return UiI18n::GetLanguage() == UiI18n::Language::English; }
std::wstring Format(const wchar_t* format, ...) {
    wchar_t text[600]{}; va_list arguments; va_start(arguments, format);
    _vsnwprintf_s(text, _countof(text), _TRUNCATE, format, arguments); va_end(arguments);
    return text;
}
std::wstring Wide(const std::string& text) { return std::wstring(text.begin(), text.end()); }

std::wstring SteamText(const NetSteam::Snapshot& s) {
    const bool en = English(); const int l = en ? 1 : 0;
    std::wstring t;
    if (s.steam == 1) t += Format(en ? L"Steam: ready (me: %ls)" : L"Steam：已就绪（我：%ls）", s.me.c_str());
    else if (s.steam < 0) t += (en ? L"Steam: unavailable — " : L"Steam：不可用——") + s.why[l];
    else t += en ? L"Steam: looking for the game's Steam…" : L"Steam：正在查找游戏的 Steam 组件…";
    t += L"\r\n";
    if (s.lobby) t += Format(en ? L"Room: %llu (%ls, %d/2)" : L"房间：%llu（%ls，%d/2 人）", static_cast<unsigned long long>(s.lobby),
                             s.owner ? (en ? L"mine" : L"我是房主") : (en ? L"joined" : L"已加入"), s.members);
    else t += s.busy ? (en ? L"Room: working…" : L"房间：处理中…") : (en ? L"Room: none" : L"房间：无");
    t += L"\r\n";
    switch (s.link) {
    case 0: t += en ? L"Peer: none" : L"对方：无"; break;
    case 1: t += Format(en ? L"Peer: %ls — handshaking…" : L"对方：%ls——正在握手…", s.peer_name.c_str()); break;
    case 2:
        t += Format(en ? L"Peer: %ls — connected" : L"对方：%ls——已连上", s.peer_name.c_str());
        if (s.rtt_ms >= 0) t += Format(en ? L", round trip %d ms" : L"，往返 %d 毫秒", s.rtt_ms);
        if (!s.route.empty()) t += L" (" + s.route + L")";
        if (s.other_mod) t += en ? L"; other mod build" : L"；对方 MOD 版本不同";
        break;
    default: t += Format(en ? L"Peer: %ls — %ls" : L"对方：%ls——%ls", s.peer_name.c_str(), s.note[l].c_str()); break;
    }
    if (!s.peer_map.empty()) t += Format(en ? L"\r\nPeer's map: %ls" : L"\r\n对方地图：%ls", Wide(s.peer_map).c_str());
    t += Format(en ? L"\r\nMessages sent %u / received %u, Steam callbacks %u, dropped %u" : L"\r\n消息：发出 %u / 收到 %u，Steam 回调 %u，丢弃 %u", s.sent, s.received, s.callbacks, s.dropped);
    if (!s.last[l].empty()) t += (en ? L"\r\nLatest: " : L"\r\n最近：") + s.last[l];
    return t;
}
std::wstring PuppetText(const NetPuppet::Snapshot& p) {
    const bool en = English(); const int l = en ? 1 : 0;
    std::wstring t;
    if (!p.alive) t += en ? L"Puppet: none" : L"替身：未生成";
    else {
        static const wchar_t* const kZh[] = {L"静止", L"逐帧照我的主角显示（本机测试）", L"逐帧照对方的主角显示"}, *const kEn[] = {L"standing", L"showing my hero pose by pose (local test)", L"showing the peer's hero pose by pose"};
        const int source = std::clamp(p.source, 0, 2);
        t += Format(en ? L"Puppet: %ls — %ls" : L"替身：%ls——%ls", Wide(p.cls).c_str(), en ? kEn[source] : kZh[source]);
    }
    if (p.alive && p.source != 0) {
        t += Format(en ? L"\r\nParts: %d (shown now %d, failed %d): body, NPC look, weapons, hair, cloth, grab partners" : L"\r\n部件：%d 个（当前显示 %d，失败 %d）：身体、NPC 外观、武器、头发、布料、投技对象",
                    p.parts, p.parts_shown, p.part_failures);
        t += Format(en ? L"\r\nPoses shown %u, part lists %u, assets not found %u" : L"\r\n已显示姿势 %u 帧，部件表 %u 次，找不到资源 %u 个", p.poses, p.parts_lists, p.assets_missing);
    }
    if (p.frames_sent)
        t += Format(en ? L"\r\nMy poses sent: %u (%d parts, %d bones per pose, %.0f KB/s)" : L"\r\n发出的姿势：%u 帧（%d 个部件，每帧 %d 根骨骼，%.0f KB/秒）", p.frames_sent, p.send_parts, p.send_bones, p.send_kbps);
    if (p.camera_fixes || p.possess_fixes) t += Format(en ? L"\r\nPut back: camera %d, player control %d" : L"\r\n纠正：镜头 %d 次，玩家控制 %d 次", p.camera_fixes, p.possess_fixes);
    if (!p.note[l].empty()) t += L"\r\n" + p.note[l];
    return t;
}

void Layout() {
    if (!window_ || !title_) return;
    const auto m = UiLayout::For(window_);
    const int x = m.Margin(), gap = m.Gap();
    RECT view{}; GetClientRect(GetParent(window_), &view);
    const int w = (std::max)(m.S(420), static_cast<int>(view.right) - 2 * x);
    int y = x;
    UiLayout::Place(title_, x, y, w, m.Section()); y += m.Section() + gap / 2;
    const int intro_h = UiLayout::TextHeight(intro_, w); UiLayout::Place(intro_, x, y, w, intro_h); y += intro_h + gap;
    const int status_h = UiLayout::TextHeight(status_, w - m.S(28), 6) + m.S(20); UiLayout::Place(status_, x, y, w, status_h); y += status_h + gap;
    UiLayout::Place(room_, x, y, w, m.Section()); y += m.Section() + gap / 2;
    y = UiLayout::ButtonRow({host_, overlay_, leave_, ping_}, x, y, w, m, m.S(120)) + gap;
    const int label_h = UiLayout::TextHeight(friends_label_, w); UiLayout::Place(friends_label_, x, y, w, label_h); y += label_h + gap / 2;
    UiLayout::Place(friends_, x, y, w, m.S(118)); y += m.S(118) + gap;
    y = UiLayout::ButtonRow({invite_, join_friend_, refresh_}, x, y, w, m, m.S(120)) + gap;
    UiLayout::Flow flow(x, y, w, m.Button(), gap, gap);
    flow.Add(room_label_, UiLayout::TextWidth(room_label_) + m.S(6), m.Line());
    flow.Add(room_id_, m.S(220), m.Row());
    flow.Add(join_, UiLayout::ButtonWidth(join_, m)); flow.Add(copy_, UiLayout::ButtonWidth(copy_, m));
    y = flow.Bottom() + gap + gap / 2;
    UiLayout::Place(puppet_, x, y, w, m.Section()); y += m.Section() + gap / 2;
    UiLayout::Flow row(x, y, w, m.Button(), gap, gap);
    row.Add(spawn_, (std::max)(m.S(120), UiLayout::ButtonWidth(spawn_, m))); row.Add(remove_, (std::max)(m.S(120), UiLayout::ButtonWidth(remove_, m)));
    row.Add(mirror_, UiLayout::CheckWidth(mirror_, m), m.Check());
    y = row.Bottom() + gap;
    const int puppet_h = UiLayout::TextHeight(puppet_status_, w - m.S(28), 4) + m.S(20); UiLayout::Place(puppet_status_, x, y, w, puppet_h); y += puppet_h + gap;
    const int note_h = UiLayout::TextHeight(note_, w); UiLayout::Place(note_, x, y, w, note_h); y += note_h;
    DebugHub::SetMinimum(window_, m.S(420) + 2 * x, y + x);
    RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}
void FillFriends(const NetSteam::Snapshot& s) {
    if (s.friends_serial == friends_serial_) return;
    friends_serial_ = s.friends_serial;
    const int selected = static_cast<int>(SendMessageW(friends_, LB_GETCURSEL, 0, 0));
    const uint64_t keep = selected >= 0 && static_cast<size_t>(selected) < friend_ids_.size() ? friend_ids_[static_cast<size_t>(selected)] : 0;
    SendMessageW(friends_, WM_SETREDRAW, FALSE, 0); SendMessageW(friends_, LB_RESETCONTENT, 0, 0); friend_ids_.clear();
    for (const auto& f : s.friends) {
        const std::wstring line = f.name + (f.lobby ? (English() ? L"  (in a room)" : L"（在房间里）") : L"");
        SendMessageW(friends_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
        friend_ids_.push_back(f.id);
        if (f.id == keep) SendMessageW(friends_, LB_SETCURSEL, friend_ids_.size() - 1, 0);
    }
    if (s.friends.empty()) {
        const std::wstring none = English() ? L"(no friend is playing this game now)" : L"（现在没有好友在玩这个游戏）";
        SendMessageW(friends_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(none.c_str()));
    }
    SendMessageW(friends_, WM_SETREDRAW, TRUE, 0); InvalidateRect(friends_, nullptr, TRUE);
}
void Refresh(bool force) {
    if (!window_) return;
    const auto s = NetSteam::Get();
    const auto p = NetPuppet::Get();
    lobby_ = s.lobby;
    FillFriends(s);
    std::wstring text = SteamText(s), puppet = PuppetText(p);
    const bool grew = std::count(text.begin(), text.end(), L'\n') != std::count(status_text_.begin(), status_text_.end(), L'\n') ||
                      std::count(puppet.begin(), puppet.end(), L'\n') != std::count(puppet_text_.begin(), puppet_text_.end(), L'\n');
    if (force || text != status_text_) { status_text_ = text; SetWindowTextW(status_, text.c_str()); }
    if (force || puppet != puppet_text_) { puppet_text_ = puppet; SetWindowTextW(puppet_status_, puppet.c_str()); }
    const bool mirror = SendMessageW(mirror_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (mirror != p.mirror) SendMessageW(mirror_, BM_SETCHECK, p.mirror ? BST_CHECKED : BST_UNCHECKED, 0);
    if (grew) Layout();
}
uint64_t SelectedFriend() {
    const int selected = static_cast<int>(SendMessageW(friends_, LB_GETCURSEL, 0, 0));
    return selected >= 0 && static_cast<size_t>(selected) < friend_ids_.size() ? friend_ids_[static_cast<size_t>(selected)] : 0;
}
bool CopyRoom() {
    if (!lobby_ || !OpenClipboard(window_)) return false;
    const std::wstring text = std::to_wstring(lobby_);
    bool ok = false;
    if (EmptyClipboard()) {
        const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
            if (void* data = GlobalLock(memory)) { memcpy(data, text.c_str(), bytes); GlobalUnlock(memory); ok = SetClipboardData(CF_UNICODETEXT, memory) != nullptr; }
            if (!ok) GlobalFree(memory);
        }
    }
    CloseClipboard();
    return ok;
}
void Command(int id) {
    switch (id) {
    case kHost: NetSteam::Ask(NetSteam::Request::Host); break;
    case kOverlay: NetSteam::Ask(NetSteam::Request::Overlay); break;
    case kLeave: NetSteam::Ask(NetSteam::Request::Leave); break;
    case kPing: NetSteam::Ask(NetSteam::Request::Ping); break;
    case kRefresh: NetSteam::Ask(NetSteam::Request::Friends); break;
    case kInvite: case kJoinFriend: {
        const uint64_t friend_id = SelectedFriend();
        if (!friend_id) { DebugHub::Notify(Tr(L"请先在列表里选一个好友").c_str()); break; }
        NetSteam::Ask(id == kInvite ? NetSteam::Request::Invite : NetSteam::Request::JoinFriend, friend_id);
        break;
    }
    case kJoin: {
        const std::wstring text = UiLayout::Caption(room_id_);
        std::wstring digits; for (wchar_t c : text) if (c >= L'0' && c <= L'9') digits += c;
        const uint64_t lobby = digits.empty() || digits.size() > 20 ? 0 : _wcstoui64(digits.c_str(), nullptr, 10);
        if (!lobby) { DebugHub::Notify(Tr(L"请输入对方的房间号").c_str()); break; }
        NetSteam::Ask(NetSteam::Request::Join, lobby);
        break;
    }
    case kCopy: DebugHub::Notify(Tr(CopyRoom() ? L"已复制房间号" : L"还没有房间号，先建立房间").c_str()); break;
    case kSpawn: NetPuppet::Ask(NetPuppet::Request::Spawn); break;
    case kRemove: NetPuppet::Ask(NetPuppet::Request::Remove); break;
    case kMirror:
        NetPuppet::Ask(SendMessageW(mirror_, BM_GETCHECK, 0, 0) == BST_CHECKED ? NetPuppet::Request::MirrorOn : NetPuppet::Request::MirrorOff);
        break;
    default: break;
    }
    next_refresh_ = GetTickCount64() + 150;
}
LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_COMMAND:
        if (LOWORD(wparam) == IDCANCEL) { DebugHub::Hide(); return 0; }
        if (HIWORD(wparam) == BN_CLICKED) Command(LOWORD(wparam));
        else if (LOWORD(wparam) == kFriends && HIWORD(wparam) == LBN_DBLCLK) Command(kInvite);
        return 0;
    case WM_SIZE: Layout(); return 0;
    case WM_DPICHANGED_AFTERPARENT: UiLayout::UseFont(window, UiTheme::GetFont(UiTheme::Font::Body, window)); Layout(); return 0;
    case WM_CLOSE: DebugHub::Hide(); return 0;
    case WM_DESTROY: window_ = nullptr; return 0;
    default: return DefWindowProcW(window, message, wparam, lparam);
    }
}
}

HWND CreatePage(HWND parent) {
    if (!parent) return nullptr;
    if (!window_) {
        WNDCLASSEXW cls{}; cls.cbSize = sizeof(cls); cls.hInstance = GetModuleHandleW(nullptr); cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.lpszClassName = kClass; cls.lpfnWndProc = &Proc; cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;
        window_ = CreateWindowExW(0, kClass, L"联机测试", WS_CHILD | WS_CLIPCHILDREN, 0, 0, 800, 650, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!window_) return nullptr;
        title_ = Control(L"STATIC", L"联机测试（第 0 阶段）", SS_LEFT);
        intro_ = Control(L"STATIC", L"需要两台电脑、两个都拥有游戏的 Steam 账号（同一个账号不能连自己）。一方建立房间并邀请，另一方在 Steam 聊天里接受邀请，或者在好友列表里加入、输入房间号加入。连上后双方各自出现一个替身，逐帧照对方主角的骨骼姿势显示：招式、格挡跳跃闪避等动作、投技（连同被抓的对象）、变身外观、武器、头发和布料都和对方一样。替身暂时不造成伤害，也不能被打。", SS_LEFT);
        status_ = Control(L"STATIC", L"", SS_LEFT, 0, false);
        room_ = Control(L"STATIC", L"房间", SS_LEFT);
        host_ = Control(L"BUTTON", L"建立房间", BS_PUSHBUTTON | WS_TABSTOP, kHost);
        overlay_ = Control(L"BUTTON", L"Steam 邀请窗口", BS_PUSHBUTTON | WS_TABSTOP, kOverlay);
        leave_ = Control(L"BUTTON", L"离开房间", BS_PUSHBUTTON | WS_TABSTOP, kLeave);
        ping_ = Control(L"BUTTON", L"发送测试消息", BS_PUSHBUTTON | WS_TABSTOP, kPing);
        friends_label_ = Control(L"STATIC", L"正在玩这个游戏的 Steam 好友（每 10 秒刷新，双击邀请）", SS_LEFT);
        friends_ = Control(L"LISTBOX", L"", WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, kFriends, false);
        invite_ = Control(L"BUTTON", L"邀请所选好友", BS_PUSHBUTTON | WS_TABSTOP, kInvite);
        join_friend_ = Control(L"BUTTON", L"加入所选好友的房间", BS_PUSHBUTTON | WS_TABSTOP, kJoinFriend);
        refresh_ = Control(L"BUTTON", L"刷新好友", BS_PUSHBUTTON | WS_TABSTOP, kRefresh);
        room_label_ = Control(L"STATIC", L"房间号", SS_LEFT | SS_CENTERIMAGE);
        room_id_ = Control(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, kRoom, false);
        SendMessageW(room_id_, EM_SETLIMITTEXT, 24, 0);
        join_ = Control(L"BUTTON", L"按房间号加入", BS_PUSHBUTTON | WS_TABSTOP, kJoin);
        copy_ = Control(L"BUTTON", L"复制我的房间号", BS_PUSHBUTTON | WS_TABSTOP, kCopy);
        puppet_ = Control(L"STATIC", L"替身", SS_LEFT);
        spawn_ = Control(L"BUTTON", L"生成替身", BS_PUSHBUTTON | WS_TABSTOP, kSpawn);
        remove_ = Control(L"BUTTON", L"移除替身", BS_PUSHBUTTON | WS_TABSTOP, kRemove);
        mirror_ = Control(L"BUTTON", L"没连上时替身模仿我", BS_AUTOCHECKBOX | WS_TABSTOP, kMirror);
        SendMessageW(mirror_, BM_SETCHECK, BST_CHECKED, 0);
        puppet_status_ = Control(L"STATIC", L"", SS_LEFT, 0, false);
        note_ = Control(L"STATIC", L"一台电脑也能先测替身：不进房间，点“生成替身”后正常出招。替身走和联机相同的数据编码，在旁边跟着显示每个招式、动作和投技。测试过程都记在游戏目录 PBZDebugMenu.log 的 NET_ 开头的行里，测完把两台电脑的这个文件发给作者。", SS_LEFT);
        UiTheme::SetRole(title_, UiTheme::Role::Section); UiTheme::SetRole(room_, UiTheme::Role::Section); UiTheme::SetRole(puppet_, UiTheme::Role::Section);
        UiTheme::SetRole(intro_, UiTheme::Role::Hint); UiTheme::SetRole(friends_label_, UiTheme::Role::Hint); UiTheme::SetRole(note_, UiTheme::Role::Hint);
        UiTheme::SetRole(status_, UiTheme::Role::Card); UiTheme::SetRole(puppet_status_, UiTheme::Role::Card);
        UiTheme::SetRole(host_, UiTheme::Role::Primary);
        language_ = UiI18n::GetLanguage();
        DebugHub::Attach(window_, parent, 460, 600);
    }
    Refresh(true); Layout(); return window_;
}
void Tick() {
    if (!window_ || !IsWindowVisible(window_)) return;
    const ULONGLONG now = GetTickCount64();
    const bool language = UiI18n::GetLanguage() != language_;
    if (!language && now < next_refresh_) return;
    next_refresh_ = now + 250;
    if (language) { friends_serial_ = ~0ull; Refresh(true); language_ = UiI18n::GetLanguage(); Layout(); return; }
    Refresh(false);
}
bool HandleCommand(const char* argument) {
    const char* space = std::strchr(argument, ' ');
    const std::string verb = space ? std::string(argument, static_cast<size_t>(space - argument)) : std::string(argument);
    const char* rest = space ? space + 1 : "";
    const uint64_t number = _strtoui64(rest, nullptr, 10);
    if (verb == "host") NetSteam::Ask(NetSteam::Request::Host);
    else if (verb == "overlay") NetSteam::Ask(NetSteam::Request::Overlay);
    else if (verb == "leave") NetSteam::Ask(NetSteam::Request::Leave);
    else if (verb == "ping") NetSteam::Ask(NetSteam::Request::Ping);
    else if (verb == "friends") NetSteam::Ask(NetSteam::Request::Friends);
    else if (verb == "join" && number) NetSteam::Ask(NetSteam::Request::Join, number);
    else if (verb == "invite" && number) NetSteam::Ask(NetSteam::Request::Invite, number);
    else if (verb == "spawn") NetPuppet::Ask(NetPuppet::Request::Spawn);
    else if (verb == "remove") NetPuppet::Ask(NetPuppet::Request::Remove);
    else if (verb == "mirror") NetPuppet::Ask(std::strcmp(rest, "0") ? NetPuppet::Request::MirrorOn : NetPuppet::Request::MirrorOff);
    else if (verb == "check") NetPuppet::Ask(NetPuppet::Request::Check);  // tests: the loop's puppet against the hero for 10 s
    else if (verb == "menu") DebugHub::ShowPage(DebugHub::Page::Online);
    else return false;
    return true;
}
void Shutdown() {
    if (window_) DestroyWindow(window_);
    window_ = nullptr; friend_ids_.clear(); friends_serial_ = ~0ull; status_text_.clear(); puppet_text_.clear();
}
}
