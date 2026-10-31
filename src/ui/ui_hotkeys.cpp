#include "ui_hotkeys.h"
#include "debug_hub.h"
#include "ui_host.h"
#include "ui_i18n.h"
#include "ui_theme.h"
#include "ui_assets.h"
#include "weapon_presets_logic.h"
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>
#include <hidsdi.h>
#include <hidpi.h>
#include <setupapi.h>
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "gdiplus.lib")

namespace UiHotkeys {
namespace {
constexpr unsigned kCtrl = 1, kShift = 2, kAlt = 4, kWin = 8;
constexpr unsigned kUp = 1u << 0, kDown = 1u << 1, kLeft = 1u << 2, kRight = 1u << 3;
constexpr unsigned kMenu = 1u << 4, kView = 1u << 5, kL3 = 1u << 6, kR3 = 1u << 7;
constexpr unsigned kLB = 1u << 8, kRB = 1u << 9, kA = 1u << 10, kB = 1u << 11, kX = 1u << 12, kY = 1u << 13;
constexpr unsigned kLT = 1u << 14, kRT = 1u << 15;
constexpr unsigned kTouch = 1u << 16;
constexpr unsigned kSwipeL = 1u << 17, kSwipeR = 1u << 18, kSwipeU = 1u << 19, kSwipeD = 1u << 20;
constexpr unsigned kSwipeMask = kSwipeL | kSwipeR | kSwipeU | kSwipeD;
constexpr int kCount = static_cast<int>(Action::Count);
constexpr wchar_t kPageClass[] = L"PBZHotkeyPage_v1";
constexpr wchar_t kBindClass[] = L"PBZHotkeyBind_v1";
constexpr COLORREF kGreen = RGB(16, 124, 16), kRed = RGB(232, 17, 35), kBlue = RGB(0, 120, 215), kYellow = RGB(255, 185, 0);
constexpr COLORREF kPsCross = RGB(90, 150, 255), kPsCircle = RGB(255, 79, 79), kPsSquare = RGB(214, 92, 204), kPsTriangle = RGB(52, 196, 96);

struct Spec { const wchar_t* id; const wchar_t* label; Chord initial; };
const Spec kSpecs[kCount] = {
    {L"ToggleHub", L"显示或隐藏面板", {Device::Keyboard, 0, VK_F10, 0}},
    {L"ShowBoss", L"Boss 连战", {}},
    {L"ShowAi", L"Boss AI", {}},
    {L"ShowCharacter", L"角色替换", {}},
    {L"ShowNpc", L"NPC 重定向", {}},
    {L"ShowAir", L"空中技能 / JC", {}},
    {L"ShowGrab", L"投技", {}},
    {L"ShowGroom", L"毛发 / Groom", {Device::Keyboard, 0, VK_F7, 0}},
    {L"ShowWeapons", L"武器预设", {}},
    {L"HairMode", L"蒙皮 / Groom", {Device::Keyboard, 0, VK_F8, 0}},
    {L"GpuBounds", L"GPU 包围盒", {Device::Keyboard, 0, VK_F9, 0}},
    {L"ForceGroom", L"强制 Groom", {Device::Keyboard, 0, VK_F6, 0}},
    {L"OutfitUnlock", L"套装解锁", {Device::Keyboard, kShift, VK_F6, 0}},
    {L"AdaptiveTriggers", L"自适应扳机", {Device::Keyboard, kShift, VK_F9, 0}},
    {L"GrabRelease", L"释放投技", {Device::PlayStation, 0, 0, kY | kB}},
    {L"WeaponSwap", L"切换武器预设", {Device::PlayStation, 0, 0, kSwipeL | kSwipeR}},
    {L"GearUp", L"提升挡位", {}},
    {L"GearDown", L"降低挡位", {}},
};

struct PadToken { unsigned bit; const wchar_t* xbox; const wchar_t* ps; };
const PadToken kPadTokens[] = {
    {kLB, L"LB", L"L1"}, {kRB, L"RB", L"R1"}, {kLT, L"LT", L"L2"}, {kRT, L"RT", L"R2"},
    {kL3, L"L3", L"L3"}, {kR3, L"R3", L"R3"},
    {kUp, L"Up", L"Up"}, {kDown, L"Down", L"Down"}, {kLeft, L"Left", L"Left"}, {kRight, L"Right", L"Right"},
    {kView, L"View", L"Create"}, {kMenu, L"Menu", L"Options"},
    {kY, L"Y", L"Triangle"}, {kX, L"X", L"Square"}, {kB, L"B", L"Circle"}, {kA, L"A", L"Cross"},
    {kTouch, L"Touch", L"Touch"},
    {kSwipeL, L"SwipeLeft", L"SwipeLeft"}, {kSwipeR, L"SwipeRight", L"SwipeRight"},
    {kSwipeU, L"SwipeUp", L"SwipeUp"}, {kSwipeD, L"SwipeDown", L"SwipeDown"},
};

enum class Mark : unsigned char { Key, Disc, Cross, Ring, Square, Triangle, Stick, Arrow, Swipe, Touch, Menu, View };
// asset >= 0 draws the game's own prompt icon (UiAssets::Id); mirror flips it
// (PS Create reuses the Options shape, as the game has no Create icon).
struct Part { Mark mark; std::wstring text; COLORREF color; int direction; int asset = -1; bool mirror = false; };

// User 10-31: page shortcuts keep their defaults (F7 = Groom) and are not
// listed or read from the ini; only action hotkeys are editable.
bool Fixed(int index) {
    return index >= static_cast<int>(Action::ShowBoss) && index <= static_cast<int>(Action::ShowWeapons);
}
// Page row of an editable action, or -1.
int RowOf(int index) {
    if (index < 0 || index >= static_cast<int>(Action::Count) || Fixed(index)) return -1;
    int row = 0;
    for (int i = 0; i < index; ++i) if (!Fixed(i)) ++row;
    return row;
}
int Rows() { return RowOf(static_cast<int>(Action::Count) - 1) + 1; }
std::array<Chord, kCount> chords_{};
std::array<std::function<void()>, kCount> handlers_{};
std::array<bool, kCount> was_{};
std::wstring config_;
bool ready_ = false;
HWND page_ = nullptr;
std::array<HWND, kCount> boxes_{};
Device pad_style_ = Device::Xbox;
ULONGLONG next_devices_ = 0;
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, void*);
XInputGetStateFn xinput_ = nullptr;
bool xinput_tried_ = false;
// Passive reader: a shared GENERIC_READ handle receives its own copy of every
// interrupt report. No GET_REPORT control requests are sent to the pad.
struct SonyDev {
    HANDLE handle = nullptr, event = nullptr; OVERLAPPED io{}; bool pending = false;
    int kind = 0; ULONG length = 64; unsigned buttons = 0; ULONGLONG last = 0; bool seen = false;
    unsigned char report[128]{}; wchar_t path[260]{};
};
SonyDev sony_[4]{};
ULONGLONG next_sony_ = 0;
std::atomic<unsigned> snoop_buttons_{0};
std::atomic<ULONGLONG> snoop_time_{0};
std::atomic<unsigned> polled_buttons_{0};
std::atomic<bool> yield_sony_{false};
HANDLE sony_thread_ = nullptr;
volatile LONG sony_stop_ = 0;
constexpr unsigned kTouchSlots = 32;
std::atomic<unsigned long long> touch_ring_[kTouchSlots]{};
std::atomic<unsigned> touch_seq_{0};
std::atomic<ULONGLONG> ordered_touch_{0};
unsigned touch_seen_ = 0;
unsigned level_ = 0;
bool level_seeded_ = false;
ULONGLONG rose_[17]{};
struct Stroke {
    bool tracking = false, wait_release = true;
    int id = -1, start_x = 0, start_y = 0, last_x = 0, last_y = 0;
    ULONGLONG start_ms = 0, last_ms = 0, last_fire = 0;
};
Stroke stroke_{};
unsigned PollSony();
unsigned TakeSwipe();
void NoteLevel(unsigned level);
bool ParseSony(int kind, const unsigned char* data, int size, unsigned& buttons);
void PublishTouch(int kind, const unsigned char* data, int size, bool ordered);
bool key_was_[256]{};
bool esc_was_ = false, wipe_was_ = false;
Chord block_{};
bool blocking_ = false;

struct Arm {
    bool active = false, ready = false, click_up = false, key_held = false, pad_held = false, live_valid = false;
    HWND control = nullptr;
    Action action = Action::ToggleHub;
    int primary = 0;
    unsigned pad_union = 0;
    Chord live{};
};
Arm arm_{};

struct XPad { WORD buttons; BYTE left; BYTE right; SHORT lx, ly, rx, ry; };
struct XState { DWORD packet; XPad pad; };
static_assert(sizeof(XState) == 16, "XInput state layout");

int Scale(HWND window, int value) {
    const UINT dpi = window ? GetDpiForWindow(window) : 96;
    return MulDiv(value, static_cast<int>(dpi ? dpi : 96), 96);
}
bool Down(int vk) { return vk > 0 && vk < 256 && (GetAsyncKeyState(vk) & 0x8000) != 0; }
unsigned Mods() {
    unsigned mods = 0;
    if (Down(VK_CONTROL)) mods |= kCtrl;
    if (Down(VK_SHIFT)) mods |= kShift;
    if (Down(VK_MENU)) mods |= kAlt;
    if (Down(VK_LWIN) || Down(VK_RWIN)) mods |= kWin;
    return mods;
}
bool Bindable(int vk) {
    switch (vk) {
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_MENU: case VK_LMENU: case VK_RMENU:
    case VK_LWIN: case VK_RWIN:
    case VK_ESCAPE: case VK_BACK: case VK_DELETE:
    case VK_LBUTTON: case VK_RBUTTON: case VK_MBUTTON: case VK_XBUTTON1: case VK_XBUTTON2:
    case VK_PACKET: case VK_PROCESSKEY:
        return false;
    default: break;
    }
    if (vk >= VK_F1 && vk <= VK_F24) return true;
    if (vk >= 'A' && vk <= 'Z') return true;
    if (vk >= '0' && vk <= '9') return true;
    if (vk >= VK_NUMPAD0 && vk <= VK_DIVIDE) return true;
    switch (vk) {
    case VK_SPACE: case VK_TAB: case VK_RETURN:
    case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT:
    case VK_PRIOR: case VK_NEXT: case VK_HOME: case VK_END: case VK_INSERT:
    case VK_SNAPSHOT: case VK_PAUSE:
    case VK_OEM_1: case VK_OEM_2: case VK_OEM_3: case VK_OEM_4: case VK_OEM_5:
    case VK_OEM_6: case VK_OEM_7: case VK_OEM_PLUS: case VK_OEM_MINUS:
    case VK_OEM_COMMA: case VK_OEM_PERIOD: case VK_OEM_102:
        return true;
    default: return false;
    }
}
bool TextEntry() {
    const HWND focus = GetFocus();
    wchar_t name[64]{};
    if (!focus || GetClassNameW(focus, name, 64) <= 0) return false;
    return !_wcsicmp(name, L"Edit") || !_wcsnicmp(name, L"RichEdit", 8) || !_wcsicmp(name, L"ComboBox") || !_wcsicmp(name, L"ComboBoxEx32");
}
std::wstring Lower(std::wstring text) {
    for (wchar_t& ch : text) ch = static_cast<wchar_t>(towlower(ch));
    return text;
}
bool SameToken(const std::wstring& token, const wchar_t* name) {
    return name && !_wcsicmp(token.c_str(), name);
}
void EnsureXInput() {
    if (xinput_tried_) return;
    xinput_tried_ = true;
    HMODULE module = LoadLibraryW(L"XInput1_4.dll");
    if (!module) module = LoadLibraryW(L"XInput1_3.dll");
    if (module) xinput_ = reinterpret_cast<XInputGetStateFn>(GetProcAddress(module, "XInputGetState"));
}
unsigned ReadPad() {
    EnsureXInput();
    unsigned mask = 0;
    if (xinput_) for (DWORD user = 0; user < 4; ++user) {
        XState state{};
        if (xinput_(user, &state) != 0) continue;
        const WORD bits = state.pad.buttons;
        if (bits & 0x0001) mask |= kUp;
        if (bits & 0x0002) mask |= kDown;
        if (bits & 0x0004) mask |= kLeft;
        if (bits & 0x0008) mask |= kRight;
        if (bits & 0x0010) mask |= kMenu;
        if (bits & 0x0020) mask |= kView;
        if (bits & 0x0040) mask |= kL3;
        if (bits & 0x0080) mask |= kR3;
        if (bits & 0x0100) mask |= kLB;
        if (bits & 0x0200) mask |= kRB;
        if (bits & 0x1000) mask |= kA;
        if (bits & 0x2000) mask |= kB;
        if (bits & 0x4000) mask |= kX;
        if (bits & 0x8000) mask |= kY;
        if (state.pad.left > 30) mask |= kLT;
        if (state.pad.right > 30) mask |= kRT;
    }
    mask |= polled_buttons_.load(std::memory_order_relaxed);
    if (GetTickCount64() - snoop_time_.load(std::memory_order_relaxed) < 150)
        mask |= snoop_buttons_.load(std::memory_order_relaxed);
    mask |= TakeSwipe();
    NoteLevel(mask);
    return mask;
}
bool ContainsI(const wchar_t* text, const wchar_t* needle) {
    if (!text || !needle || !*needle) return false;
    const size_t span = wcslen(needle);
    for (const wchar_t* at = text; *at; ++at)
        if (!_wcsnicmp(at, needle, span)) return true;
    return false;
}
void RefreshDevices() {
    if (GetTickCount64() < next_devices_) return;
    next_devices_ = GetTickCount64() + 1500;
    UINT count = 0;
    if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) != 0 || !count) {
        pad_style_ = Device::Xbox;
        return;
    }
    std::vector<RAWINPUTDEVICELIST> devices(count);
    if (GetRawInputDeviceList(devices.data(), &count, sizeof(RAWINPUTDEVICELIST)) == static_cast<UINT>(-1)) return;
    bool sony = false, xbox = false;
    for (UINT i = 0; i < count; ++i) {
        if (devices[i].dwType != RIM_TYPEHID) continue;
        wchar_t name[256]{};
        UINT bytes = sizeof(name);
        if (GetRawInputDeviceInfoW(devices[i].hDevice, RIDI_DEVICENAME, name, &bytes) == static_cast<UINT>(-1)) continue;
        if (ContainsI(name, L"VID_054C")) sony = true;
        if (ContainsI(name, L"VID_045E")) xbox = true;
    }
    // XInput cannot tell two pads apart. A lone DualSense (no Xbox HID) keeps the PS glyphs.
    pad_style_ = sony && !xbox ? Device::PlayStation : Device::Xbox;
}
bool ParseSony(int kind, const unsigned char* data, int size, unsigned& buttons) {
    if (!data || size < 10 || (kind != 1 && kind != 2)) return false;
    const bool dualsense = kind == 1;
    int base = -1;
    if (data[0] == 0x01) base = 1;
    else if (data[0] == 0x31 && dualsense) base = 2;
    else if (data[0] == 0x11 && !dualsense) base = 3;
    else return false;
    if (size < base + (dualsense ? 10 : 9)) return false;
    const unsigned b0 = data[base + (dualsense ? 7 : 4)];
    const unsigned b1 = data[base + (dualsense ? 8 : 5)];
    const unsigned l2 = data[base + (dualsense ? 4 : 7)];
    const unsigned r2 = data[base + (dualsense ? 5 : 8)];
    unsigned mask = 0;
    const unsigned hat = b0 & 0x0f;
    if (hat <= 7) {
        if (hat == 0 || hat == 1 || hat == 7) mask |= kUp;
        if (hat >= 1 && hat <= 3) mask |= kRight;
        if (hat >= 3 && hat <= 5) mask |= kDown;
        if (hat >= 5 && hat <= 7) mask |= kLeft;
    }
    if (b0 & 0x10) mask |= kX;
    if (b0 & 0x20) mask |= kA;
    if (b0 & 0x40) mask |= kB;
    if (b0 & 0x80) mask |= kY;
    if (b1 & 0x01) mask |= kLB;
    if (b1 & 0x02) mask |= kRB;
    if ((b1 & 0x04) || l2 > 30) mask |= kLT;
    if ((b1 & 0x08) || r2 > 30) mask |= kRT;
    if (b1 & 0x10) mask |= kView;
    if (b1 & 0x20) mask |= kMenu;
    if (b1 & 0x40) mask |= kL3;
    if (b1 & 0x80) mask |= kR3;
    const unsigned b2 = data[base + (dualsense ? 9 : 6)];
    if (b2 & 0x02) mask |= kTouch;
    buttons = mask;
    return true;
}
void PublishTouch(int kind, const unsigned char* data, int size, bool ordered) {
    if (kind != 1 || !data || size <= 0) return;
    const ULONGLONG now = GetTickCount64();
    if (ordered) ordered_touch_.store(now, std::memory_order_relaxed);
    else if (now - ordered_touch_.load(std::memory_order_relaxed) < 500) return;
    WeaponPresetsLogic::Touch touch{};
    if (!WeaponPresetsLogic::DecodeTouch(reinterpret_cast<const uint8_t*>(data), static_cast<size_t>(size), touch)) return;
    const unsigned seq = touch_seq_.fetch_add(1, std::memory_order_relaxed) + 1;
    unsigned long long word = touch.down ? 1ull : 0ull;
    word |= (static_cast<unsigned long long>(touch.id) & 0x7f) << 1;
    word |= (static_cast<unsigned long long>(touch.x) & 0xfff) << 8;
    word |= (static_cast<unsigned long long>(touch.y) & 0xfff) << 20;
    word |= (static_cast<unsigned long long>(seq) & 0xFFFFFull) << 44;
    touch_ring_[seq & (kTouchSlots - 1)].store(word, std::memory_order_release);
}
unsigned ClassifyStroke(ULONGLONG now) {
    if (now < stroke_.start_ms || now - stroke_.start_ms > 900) return 0;
    if (stroke_.last_fire && now - stroke_.last_fire < 400) return 0;
    const int dx = stroke_.last_x - stroke_.start_x;
    const int dy = stroke_.last_y - stroke_.start_y;
    const int adx = dx < 0 ? -dx : dx;
    const int ady = dy < 0 ? -dy : dy;
    unsigned bit = 0;
    if (adx >= 320 && ady <= 220 && adx > ady * 2) bit = dx < 0 ? kSwipeL : kSwipeR;
    else if (ady >= 240 && adx <= 220 && ady > adx * 2) bit = dy < 0 ? kSwipeU : kSwipeD;
    if (bit) stroke_.last_fire = now;
    return bit;
}
unsigned FeedWord(unsigned long long word, ULONGLONG now) {
    const bool down = (word & 1ull) != 0;
    const int id = static_cast<int>((word >> 1) & 0x7f);
    const int x = static_cast<int>((word >> 8) & 0xfff);
    const int y = static_cast<int>((word >> 20) & 0xfff);
    if (!down) {
        const unsigned bit = stroke_.tracking ? ClassifyStroke(now) : 0;
        stroke_.tracking = false;
        stroke_.wait_release = false;
        stroke_.last_ms = now;
        return bit;
    }
    if (stroke_.wait_release) { stroke_.last_ms = now; return 0; }
    if (!stroke_.tracking || id != stroke_.id) {
        stroke_.tracking = true;
        stroke_.id = id;
        stroke_.start_x = x;
        stroke_.start_y = y;
        stroke_.start_ms = now;
    }
    stroke_.last_x = x;
    stroke_.last_y = y;
    stroke_.last_ms = now;
    return 0;
}
unsigned TakeSwipe() {
    const unsigned latest = touch_seq_.load(std::memory_order_acquire);
    const ULONGLONG now = GetTickCount64();
    if (latest == touch_seen_) {
        if (stroke_.tracking && stroke_.last_ms && now - stroke_.last_ms > 300) {
            stroke_.tracking = false;
            stroke_.wait_release = true;
        }
        return 0;
    }
    unsigned from = touch_seen_;
    if (latest - touch_seen_ > kTouchSlots) {
        stroke_.tracking = false;
        stroke_.wait_release = true;
        from = latest - kTouchSlots;
    }
    if (stroke_.last_ms && now - stroke_.last_ms > 300) {
        stroke_.tracking = false;
        stroke_.wait_release = true;
    }
    unsigned fired = 0;
    for (unsigned seq = from + 1;; ++seq) {
        const unsigned long long word = touch_ring_[seq & (kTouchSlots - 1)].load(std::memory_order_acquire);
        const unsigned got = static_cast<unsigned>((word >> 44) & 0xFFFFFull);
        if (got == (seq & 0xFFFFFull)) {
            const unsigned bit = FeedWord(word, now);
            if (bit) fired = bit;
        }
        if (seq == latest) break;
    }
    touch_seen_ = latest;
    return fired;
}
void NoteLevel(unsigned level) {
    level &= ~kSwipeMask;
    if (!level_seeded_) { level_ = level; level_seeded_ = true; return; }
    const unsigned rose = level & ~level_;
    if (rose) {
        const ULONGLONG now = GetTickCount64();
        for (unsigned i = 0; i <= 16; ++i) if (rose & (1u << i)) rose_[i] = now;
    }
    level_ = level;
}
bool RisenTogether(unsigned bits) {
    int count = 0;
    ULONGLONG oldest = ~0ull, newest = 0;
    for (unsigned i = 0; i <= 16; ++i) {
        if (!(bits & (1u << i))) continue;
        const ULONGLONG at = rose_[i];
        if (!at) return false;
        if (at < oldest) oldest = at;
        if (at > newest) newest = at;
        ++count;
    }
    return count <= 1 || newest - oldest <= 220;
}
int SonyKind(const wchar_t* path) {
    if (!ContainsI(path, L"054C")) return 0;
    if (ContainsI(path, L"PID_0CE6") || ContainsI(path, L"PID&0CE6") || ContainsI(path, L"PID_0DF2") || ContainsI(path, L"PID&0DF2")) return 1;
    if (ContainsI(path, L"PID_05C4") || ContainsI(path, L"PID&05C4") || ContainsI(path, L"PID_09CC") || ContainsI(path, L"PID&09CC") ||
        ContainsI(path, L"PID_0BA0") || ContainsI(path, L"PID&0BA0")) return 2;
    return 0;
}
void CloseDev(SonyDev& dev) {
    if (dev.handle) {
        if (dev.pending) { CancelIoEx(dev.handle, &dev.io); DWORD bytes = 0; GetOverlappedResult(dev.handle, &dev.io, &bytes, TRUE); }
        CloseHandle(dev.handle);
    }
    if (dev.event) CloseHandle(dev.event);
    dev = {};
}
bool Issue(SonyDev& dev) {
    std::memset(&dev.io, 0, sizeof(dev.io)); dev.io.hEvent = dev.event; ResetEvent(dev.event);
    DWORD bytes = 0;
    dev.pending = ReadFile(dev.handle, dev.report, dev.length, &bytes, &dev.io) != FALSE || GetLastError() == ERROR_IO_PENDING;
    return dev.pending;
}
unsigned PollSony() {
    const ULONGLONG now = GetTickCount64();
    if (now >= next_sony_) {
        next_sony_ = now + 1500;
        for (SonyDev& dev : sony_) dev.seen = false;
        GUID guid{};
        HidD_GetHidGuid(&guid);
        const HDEVINFO set = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (set != INVALID_HANDLE_VALUE) {
            SP_DEVICE_INTERFACE_DATA iface{sizeof(iface)};
            for (DWORD index = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &guid, index, &iface); ++index) {
                DWORD bytes = 0;
                SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &bytes, nullptr);
                if (!bytes || bytes > 4096) continue;
                std::vector<unsigned char> block(bytes);
                auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(block.data());
                detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
                if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, bytes, nullptr, nullptr)) continue;
                const int kind = SonyKind(detail->DevicePath);
                if (!kind) continue;
                SonyDev* slot = nullptr;
                for (SonyDev& dev : sony_) if (dev.handle && !_wcsicmp(dev.path, detail->DevicePath)) { dev.seen = true; slot = &dev; break; }
                if (slot) continue;
                for (SonyDev& dev : sony_) if (!dev.handle) { slot = &dev; break; }
                if (!slot) continue;
                HANDLE handle = CreateFileW(detail->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
                if (handle == INVALID_HANDLE_VALUE) continue;
                ULONG length = 64;
                USHORT page = 0, usage = 0;
                PHIDP_PREPARSED_DATA preparsed = nullptr;
                if (HidD_GetPreparsedData(handle, &preparsed)) {
                    HIDP_CAPS caps{};
                    if (HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS) {
                        page = caps.UsagePage;
                        usage = caps.Usage;
                        if (caps.InputReportByteLength >= 8 && caps.InputReportByteLength <= 128) length = caps.InputReportByteLength;
                    }
                    HidD_FreePreparsedData(preparsed);
                }
                if (page != 0x01 || (usage != 0x04 && usage != 0x05)) { CloseHandle(handle); continue; }
                slot->handle = handle;
                slot->event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                slot->kind = kind;
                slot->length = length;
                slot->seen = true;
                slot->buttons = 0;
                slot->last = 0;
                wcsncpy_s(slot->path, detail->DevicePath, _TRUNCATE);
                if (!slot->event || !Issue(*slot)) CloseDev(*slot);
            }
            SetupDiDestroyDeviceInfoList(set);
        }
        for (SonyDev& dev : sony_) if (dev.handle && !dev.seen) CloseDev(dev);
    }
    HANDLE events[4]{}; DWORD waiting = 0;
    for (SonyDev& dev : sony_) if (dev.handle && dev.pending) events[waiting++] = dev.event;
    if (!waiting) Sleep(20);
    else WaitForMultipleObjects(waiting, events, FALSE, 20);
    unsigned mask = 0;
    for (SonyDev& dev : sony_) {
        if (!dev.handle) continue;
        if (dev.pending && WaitForSingleObject(dev.event, 0) == WAIT_OBJECT_0) {
            DWORD bytes = 0;
            dev.pending = false;
            if (!GetOverlappedResult(dev.handle, &dev.io, &bytes, FALSE)) { CloseDev(dev); continue; }
            unsigned buttons = 0;
            if (ParseSony(dev.kind, dev.report, static_cast<int>(bytes), buttons)) {
                dev.buttons = buttons; dev.last = GetTickCount64();
                if (dev.kind == 1) PublishTouch(dev.kind, dev.report, static_cast<int>(bytes), false);
            }
            if (!Issue(dev)) { CloseDev(dev); continue; }
        }
        if (GetTickCount64() - dev.last < 200) mask |= dev.buttons;
    }
    return mask;
}
void CloseSony() {
    for (SonyDev& dev : sony_) if (dev.handle) CloseDev(dev);
}
DWORD WINAPI SonyThread(void*) {
    while (!InterlockedCompareExchange(&sony_stop_, 0, 0)) {
        if (yield_sony_.load(std::memory_order_relaxed)) {
            CloseSony();
            polled_buttons_.store(0, std::memory_order_relaxed);
            Sleep(30);
            continue;
        }
        polled_buttons_.store(PollSony(), std::memory_order_relaxed);
    }
    CloseSony();
    polled_buttons_.store(0, std::memory_order_relaxed);
    return 0;
}
bool Conflicts(const Chord& left, const Chord& right) {
    if (left.empty() || right.empty()) return false;
    const bool left_pad = left.device != Device::Keyboard, right_pad = right.device != Device::Keyboard;
    if (left_pad && right_pad) {
        const unsigned as = left.buttons & kSwipeMask, bs = right.buttons & kSwipeMask;
        const unsigned ab = left.buttons & ~kSwipeMask, bb = right.buttons & ~kSwipeMask;
        if (ab != bb) return false;
        if (!as && !bs) return true;
        if (!as || !bs) return false;
        return (as & bs) != 0;
    }
    if (!left_pad && !right_pad) return left.key == right.key && left.modifiers == right.modifiers;
    return false;
}
bool SwipeChoice(const Chord& chord) {
    const unsigned swipe = chord.buttons & kSwipeMask;
    return swipe && !(chord.buttons & ~kSwipeMask) && (swipe & (swipe - 1)) != 0;
}
bool Satisfied(const Chord& chord, unsigned mods, unsigned pad) {
    if (chord.empty()) return false;
    if (chord.device == Device::Keyboard) return chord.key && Down(static_cast<int>(chord.key)) && mods == chord.modifiers;
    const unsigned want_swipe = chord.buttons & kSwipeMask;
    const unsigned want_bits = chord.buttons & ~kSwipeMask;
    const unsigned have_swipe = pad & kSwipeMask;
    const unsigned have_bits = pad & ~kSwipeMask;
    if (want_swipe && !want_bits) {
        if (have_swipe & ~want_swipe) return false;
        return (have_swipe & want_swipe) != 0;
    }
    if (!chord.buttons || have_bits != want_bits || (have_swipe & ~want_swipe) || !RisenTogether(want_bits)) return false;
    if (!want_swipe) return true;
    return (have_swipe & want_swipe) != 0;
}
std::wstring KeyToken(unsigned vk) {
    if (vk >= VK_F1 && vk <= VK_F24) return L"F" + std::to_wstring(vk - VK_F1 + 1);
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::wstring(1, static_cast<wchar_t>(vk));
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return L"num" + std::to_wstring(vk - VK_NUMPAD0);
    switch (vk) {
    case VK_SPACE: return L"space"; case VK_TAB: return L"tab"; case VK_RETURN: return L"enter";
    case VK_UP: return L"up"; case VK_DOWN: return L"down"; case VK_LEFT: return L"left"; case VK_RIGHT: return L"right";
    case VK_PRIOR: return L"pgup"; case VK_NEXT: return L"pgdn"; case VK_HOME: return L"home"; case VK_END: return L"end";
    case VK_INSERT: return L"insert"; case VK_SNAPSHOT: return L"print"; case VK_PAUSE: return L"pause";
    case VK_OEM_PLUS: return L"plus"; case VK_OEM_MINUS: return L"minus"; case VK_OEM_COMMA: return L"comma";
    case VK_OEM_PERIOD: return L"period"; case VK_MULTIPLY: return L"nummul"; case VK_ADD: return L"numadd";
    case VK_SUBTRACT: return L"numsub"; case VK_DECIMAL: return L"numdot"; case VK_DIVIDE: return L"numdiv";
    default: return L"vk" + std::to_wstring(vk);
    }
}
unsigned ParseKey(const std::wstring& token) {
    const std::wstring lower = Lower(token);
    if (lower.size() > 1 && lower[0] == L'f') {
        const int number = _wtoi(lower.c_str() + 1);
        if (number >= 1 && number <= 24 && lower == L"f" + std::to_wstring(number)) return static_cast<unsigned>(VK_F1 + number - 1);
    }
    if (lower.rfind(L"vk", 0) == 0) {
        const int number = _wtoi(lower.c_str() + 2);
        if (number > 0 && number < 256 && lower == L"vk" + std::to_wstring(number)) return static_cast<unsigned>(number);
    }
    if (lower.rfind(L"num", 0) == 0 && lower.size() == 4 && lower[3] >= L'0' && lower[3] <= L'9')
        return static_cast<unsigned>(VK_NUMPAD0 + (lower[3] - L'0'));
    if (lower.size() == 1 && lower[0] >= L'a' && lower[0] <= L'z') return static_cast<unsigned>(lower[0] - L'a' + 'A');
    if (lower.size() == 1 && lower[0] >= L'0' && lower[0] <= L'9') return static_cast<unsigned>(lower[0]);
    if (lower == L"space") return VK_SPACE;
    if (lower == L"tab") return VK_TAB;
    if (lower == L"enter" || lower == L"return") return VK_RETURN;
    if (lower == L"up") return VK_UP;
    if (lower == L"down") return VK_DOWN;
    if (lower == L"left") return VK_LEFT;
    if (lower == L"right") return VK_RIGHT;
    if (lower == L"pgup") return VK_PRIOR;
    if (lower == L"pgdn") return VK_NEXT;
    if (lower == L"home") return VK_HOME;
    if (lower == L"end") return VK_END;
    if (lower == L"insert" || lower == L"ins") return VK_INSERT;
    if (lower == L"print") return VK_SNAPSHOT;
    if (lower == L"pause") return VK_PAUSE;
    if (lower == L"plus") return VK_OEM_PLUS;
    if (lower == L"minus") return VK_OEM_MINUS;
    if (lower == L"comma") return VK_OEM_COMMA;
    if (lower == L"period") return VK_OEM_PERIOD;
    if (lower == L"nummul") return VK_MULTIPLY;
    if (lower == L"numadd") return VK_ADD;
    if (lower == L"numsub") return VK_SUBTRACT;
    if (lower == L"numdot") return VK_DECIMAL;
    if (lower == L"numdiv") return VK_DIVIDE;
    return 0;
}
unsigned ModToken(const std::wstring& token) {
    const std::wstring lower = Lower(token);
    if (lower == L"ctrl" || lower == L"control") return kCtrl;
    if (lower == L"shift") return kShift;
    if (lower == L"alt") return kAlt;
    if (lower == L"win") return kWin;
    return 0;
}
unsigned PadBit(const std::wstring& token) {
    for (const PadToken& item : kPadTokens) {
        if (SameToken(token, item.xbox) || SameToken(token, item.ps)) return item.bit;
    }
    if (SameToken(token, L"Start")) return kMenu;
    if (SameToken(token, L"Back") || SameToken(token, L"Share")) return kView;
    if (SameToken(token, L"LSB")) return kL3;
    if (SameToken(token, L"RSB")) return kR3;
    return 0;
}
bool Parse(const wchar_t* text, Chord& out) {
    out = {};
    if (!text || !*text) return true;
    Device device = Device::Keyboard;
    const wchar_t* body = text;
    if (!_wcsnicmp(text, L"kb:", 3)) device = Device::Keyboard;
    else if (!_wcsnicmp(text, L"xb:", 3)) device = Device::Xbox;
    else if (!_wcsnicmp(text, L"ps:", 3)) device = Device::PlayStation;
    else return false;
    body += 3;
    Chord chord; chord.device = device;
    const std::wstring value(body);
    size_t at = 0;
    while (at <= value.size()) {
        const size_t plus = value.find(L'+', at);
        const std::wstring token = value.substr(at, plus == std::wstring::npos ? std::wstring::npos : plus - at);
        if (token.empty()) return false;
        if (device == Device::Keyboard) {
            const unsigned mod = ModToken(token);
            if (mod) chord.modifiers |= mod;
            else {
                const unsigned key = ParseKey(token);
                if (!key || chord.key) return false;
                chord.key = key;
            }
        } else {
            const unsigned bit = PadBit(token);
            if (!bit || (chord.buttons & bit)) return false;
            chord.buttons |= bit;
        }
        if (plus == std::wstring::npos) break;
        at = plus + 1;
    }
    if (device == Device::Keyboard ? chord.key == 0 : chord.buttons == 0) return false;
    out = chord;
    return true;
}
std::wstring Format(const Chord& chord) {
    if (chord.empty()) return L"";
    std::wstring text = chord.device == Device::PlayStation ? L"ps:" : chord.device == Device::Xbox ? L"xb:" : L"kb:";
    if (chord.device == Device::Keyboard) {
        if (chord.modifiers & kCtrl) text += L"ctrl+";
        if (chord.modifiers & kShift) text += L"shift+";
        if (chord.modifiers & kAlt) text += L"alt+";
        if (chord.modifiers & kWin) text += L"win+";
        text += KeyToken(chord.key);
        return text;
    }
    bool first = true;
    for (const PadToken& item : kPadTokens) if (chord.buttons & item.bit) {
        if (!first) text += L"+";
        first = false;
        text += chord.device == Device::PlayStation ? item.ps : item.xbox;
    }
    return text;
}
void Save(int index) {
    if (config_.empty() || index < 0 || index >= kCount) return;
    WritePrivateProfileStringW(L"Hotkeys", kSpecs[index].id, Format(chords_[static_cast<size_t>(index)]).c_str(), config_.c_str());
}
void Ensure() {
    if (ready_) return;
    ready_ = true;
    for (int i = 0; i < kCount; ++i) chords_[static_cast<size_t>(i)] = kSpecs[i].initial;
}
std::wstring KeyCaption(unsigned vk) {
    const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    if (!scan) return KeyToken(vk);
    LONG flags = static_cast<LONG>(scan) << 16;
    switch (vk) {
    case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN:
    case VK_PRIOR: case VK_NEXT: case VK_HOME: case VK_END:
    case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK:
        flags |= 1 << 24; break;
    default: break;
    }
    wchar_t text[64]{};
    if (GetKeyNameTextW(flags, text, 64) > 0 && text[0]) return text;
    return KeyToken(vk);
}
void AddPad(std::vector<Part>& parts, const Chord& chord) {
    const bool ps = chord.device == Device::PlayStation;
    const bool mono = UiTheme::HighContrast();
    using UiAssets::Id;
    struct Art { unsigned bit; Id ps, xbox; bool mirror; };
    static const Art kArt[] = {
        {kLB, Id::PsL1, Id::XbLB, false}, {kRB, Id::PsR1, Id::XbRB, false}, {kLT, Id::PsL2, Id::XbLT, false}, {kRT, Id::PsR2, Id::XbRT, false},
        {kL3, Id::PsL3, Id::XbL3, false}, {kR3, Id::PsR3, Id::XbR3, false},
        {kUp, Id::PsUp, Id::XbUp, false}, {kDown, Id::PsDown, Id::XbDown, false}, {kLeft, Id::PsLeft, Id::XbLeft, false}, {kRight, Id::PsRight, Id::XbRight, false},
        {kMenu, Id::PsOptions, Id::XbMenu, false}, {kView, Id::PsOptions, Id::Count, true},
        {kY, Id::PsTriangle, Id::XbY, false}, {kX, Id::PsSquare, Id::XbX, false}, {kB, Id::PsCircle, Id::XbB, false}, {kA, Id::PsCross, Id::XbA, false},
        {kTouch, Id::PsTouch, Id::PsTouch, false},
        {kSwipeU, Id::PsSwipeUp, Id::PsSwipeUp, false}, {kSwipeD, Id::PsSwipeDown, Id::PsSwipeDown, false},
        {kSwipeL, Id::PsSwipeLeft, Id::PsSwipeLeft, false}, {kSwipeR, Id::PsSwipeRight, Id::PsSwipeRight, false},
    };
    auto add = [&](unsigned bit, Mark xbox, Mark play, const wchar_t* xbox_text, const wchar_t* ps_text, COLORREF color, int direction) {
        if (!(chord.buttons & bit)) return;
        Part part;
        part.mark = mono ? Mark::Key : (ps ? play : xbox);
        part.text = ps ? ps_text : xbox_text;
        part.color = mono ? UiTheme::Text() : color;
        part.direction = direction;
        if (!mono) for (const Art& art : kArt) if (art.bit == bit) {
            const Id id = ps ? art.ps : art.xbox;
            if (id != Id::Count) { part.asset = static_cast<int>(id); part.mirror = ps && art.mirror; }
        }
        parts.push_back(std::move(part));
    };
    add(kLB, Mark::Key, Mark::Key, L"LB", L"L1", 0, 0);
    add(kRB, Mark::Key, Mark::Key, L"RB", L"R1", 0, 0);
    add(kLT, Mark::Key, Mark::Key, L"LT", L"L2", 0, 0);
    add(kRT, Mark::Key, Mark::Key, L"RT", L"R2", 0, 0);
    add(kL3, Mark::Stick, Mark::Stick, L"L", L"L", UiTheme::Text(), 0);
    add(kR3, Mark::Stick, Mark::Stick, L"R", L"R", UiTheme::Text(), 0);
    add(kUp, Mark::Arrow, Mark::Arrow, L"Up", L"Up", UiTheme::Text(), 0);
    add(kDown, Mark::Arrow, Mark::Arrow, L"Down", L"Down", UiTheme::Text(), 1);
    add(kLeft, Mark::Arrow, Mark::Arrow, L"Left", L"Left", UiTheme::Text(), 2);
    add(kRight, Mark::Arrow, Mark::Arrow, L"Right", L"Right", UiTheme::Text(), 3);
    add(kView, Mark::View, Mark::View, L"View", L"Create", UiTheme::Text(), 0);
    add(kMenu, Mark::Menu, Mark::Menu, L"Menu", L"Options", UiTheme::Text(), 0);
    add(kY, Mark::Disc, Mark::Triangle, L"Y", L"Triangle", kYellow, 0);
    add(kX, Mark::Disc, Mark::Square, L"X", L"Square", kBlue, 0);
    add(kB, Mark::Disc, Mark::Ring, L"B", L"Circle", kRed, 0);
    add(kA, Mark::Disc, Mark::Cross, L"A", L"Cross", ps ? kPsCross : kGreen, 0);
    add(kTouch, Mark::Touch, Mark::Touch, L"触控板", L"触控板", UiTheme::Text(), 0);
    add(kSwipeU, Mark::Swipe, Mark::Swipe, L"上滑", L"上滑", UiTheme::Text(), 0);
    add(kSwipeD, Mark::Swipe, Mark::Swipe, L"下滑", L"下滑", UiTheme::Text(), 1);
    add(kSwipeL, Mark::Swipe, Mark::Swipe, L"左滑", L"左滑", UiTheme::Text(), 2);
    add(kSwipeR, Mark::Swipe, Mark::Swipe, L"右滑", L"右滑", UiTheme::Text(), 3);
    if (ps) {
        for (Part& part : parts) {
            if (part.mark == Mark::Triangle) part.color = kPsTriangle;
            else if (part.mark == Mark::Square) part.color = kPsSquare;
            else if (part.mark == Mark::Ring) part.color = kPsCircle;
            else if (part.mark == Mark::Cross) part.color = kPsCross;
        }
    }
}
std::vector<Part> Parts(const Chord& chord) {
    std::vector<Part> parts;
    if (chord.empty()) return parts;
    if (chord.device == Device::Keyboard) {
        auto cap = [](unsigned vk) { Part part; part.mark = Mark::Key; part.text = KeyCaption(vk); part.color = UiTheme::Text(); return part; };
        if (chord.modifiers & kCtrl) parts.push_back(cap(VK_CONTROL));
        if (chord.modifiers & kShift) parts.push_back(cap(VK_SHIFT));
        if (chord.modifiers & kAlt) parts.push_back(cap(VK_MENU));
        if (chord.modifiers & kWin) parts.push_back(cap(VK_LWIN));
        if (chord.key) parts.push_back(cap(chord.key));
        return parts;
    }
    AddPad(parts, chord);
    return parts;
}
Gdiplus::Color Rgb(COLORREF color, BYTE alpha = 255) { return Gdiplus::Color(alpha, GetRValue(color), GetGValue(color), GetBValue(color)); }
void Round(Gdiplus::Graphics& graphics, const RECT& rect, float radius, COLORREF fill, COLORREF edge) {
    const float x = static_cast<float>(rect.left), y = static_cast<float>(rect.top);
    const float w = static_cast<float>(rect.right - rect.left), h = static_cast<float>(rect.bottom - rect.top);
    Gdiplus::GraphicsPath path;
    const float d = (std::min)(radius * 2.f, (std::min)(w, h));
    path.AddArc(x, y, d, d, 180, 90); path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90); path.AddArc(x, y + h - d, d, d, 90, 90); path.CloseFigure();
    Gdiplus::SolidBrush brush(Rgb(fill));
    graphics.FillPath(&brush, &path);
    Gdiplus::Pen pen(Rgb(edge), 1.f);
    graphics.DrawPath(&pen, &path);
}
void Glyph(Gdiplus::Graphics& graphics, const RECT& rect, const Part& part, COLORREF paper, COLORREF ink, COLORREF edge) {
    const float left = static_cast<float>(rect.left), top = static_cast<float>(rect.top);
    const float right = static_cast<float>(rect.right), bottom = static_cast<float>(rect.bottom);
    const float w = right - left, h = bottom - top, inset = h * .22f;
    if (part.mark == Mark::Disc) {
        Gdiplus::SolidBrush brush(Rgb(part.color));
        graphics.FillEllipse(&brush, left, top, w - 1.f, h - 1.f);
        return;
    }
    if (part.mark == Mark::Stick || part.mark == Mark::Ring || part.mark == Mark::Square || part.mark == Mark::Triangle || part.mark == Mark::Cross || part.mark == Mark::Arrow || part.mark == Mark::Swipe || part.mark == Mark::Touch) {
        Round(graphics, rect, h * .28f, paper, edge);
    }
    Gdiplus::Pen pen(Rgb(part.color ? part.color : ink), (std::max)(1.6f, h * .12f));
    pen.SetStartCap(Gdiplus::LineCapRound); pen.SetEndCap(Gdiplus::LineCapRound); pen.SetLineJoin(Gdiplus::LineJoinRound);
    if (part.mark == Mark::Cross) {
        graphics.DrawLine(&pen, left + inset, top + inset, right - inset, bottom - inset);
        graphics.DrawLine(&pen, right - inset, top + inset, left + inset, bottom - inset);
    } else if (part.mark == Mark::Ring) {
        graphics.DrawEllipse(&pen, left + inset, top + inset, w - inset * 2, h - inset * 2);
    } else if (part.mark == Mark::Square) {
        graphics.DrawRectangle(&pen, left + inset, top + inset, w - inset * 2, h - inset * 2);
    } else if (part.mark == Mark::Triangle) {
        const Gdiplus::PointF points[3] = {{left + w / 2.f, top + inset * .7f}, {right - inset, bottom - inset}, {left + inset, bottom - inset}};
        graphics.DrawPolygon(&pen, points, 3);
    } else if (part.mark == Mark::Stick) {
        graphics.DrawEllipse(&pen, left + inset, top + inset, w - inset * 2, h - inset * 2);
    } else if (part.mark == Mark::Arrow) {
        const float m = inset;
        Gdiplus::PointF points[3] = {{left + w / 2.f, top + m}, {right - m, bottom - m}, {left + m, bottom - m}};
        if (part.direction == 1) { points[0] = {left + w / 2.f, bottom - m}; points[1] = {left + m, top + m}; points[2] = {right - m, top + m}; }
        else if (part.direction == 2) { points[0] = {left + m, top + h / 2.f}; points[1] = {right - m, top + m}; points[2] = {right - m, bottom - m}; }
        else if (part.direction == 3) { points[0] = {right - m, top + h / 2.f}; points[1] = {left + m, top + m}; points[2] = {left + m, bottom - m}; }
        Gdiplus::SolidBrush brush(Rgb(part.color ? part.color : ink));
        graphics.FillPolygon(&brush, points, 3);
    } else if (part.mark == Mark::Swipe) {
        const float m = inset;
        Gdiplus::PointF points[3] = {{left + w / 2.f, top + m}, {right - m, bottom - m}, {left + m, bottom - m}};
        if (part.direction == 1) { points[0] = {left + w / 2.f, bottom - m}; points[1] = {left + m, top + m}; points[2] = {right - m, top + m}; }
        else if (part.direction == 2) { points[0] = {left + m, top + h / 2.f}; points[1] = {right - m, top + m}; points[2] = {right - m, bottom - m}; }
        else if (part.direction == 3) { points[0] = {right - m, top + h / 2.f}; points[1] = {left + m, top + m}; points[2] = {left + m, bottom - m}; }
        graphics.DrawPolygon(&pen, points, 3);
    } else if (part.mark == Mark::Touch) {
        Gdiplus::SolidBrush brush(Rgb(ink));
        const float d = (std::max)(3.f, h * .18f);
        graphics.FillEllipse(&brush, left + (w - d) / 2.f, top + (h - d) / 2.f, d, d);
    } else if (part.mark == Mark::Menu || part.mark == Mark::View) {
        Round(graphics, rect, h * .28f, paper, edge);
        Gdiplus::Pen line(Rgb(ink), (std::max)(1.4f, h * .08f));
        line.SetStartCap(Gdiplus::LineCapRound); line.SetEndCap(Gdiplus::LineCapRound);
        if (part.mark == Mark::Menu) {
            for (int i = 0; i < 3; ++i) {
                const float y = top + h * (.32f + .18f * static_cast<float>(i));
                graphics.DrawLine(&line, left + w * .28f, y, right - w * .28f, y);
            }
        } else if (part.text == L"Create") {
            graphics.DrawLine(&line, left + w * .28f, top + h * .5f, right - w * .28f, top + h * .5f);
            graphics.DrawLine(&line, left + w * .34f, top + h * .36f, left + w * .34f, top + h * .64f);
        } else {
            Gdiplus::Pen box(Rgb(ink), (std::max)(1.4f, h * .08f));
            graphics.DrawRectangle(&box, left + w * .26f, top + h * .3f, w * .28f, h * .4f);
            graphics.DrawRectangle(&box, left + w * .46f, top + h * .3f, w * .28f, h * .4f);
        }
    } else {
        Round(graphics, rect, h * .28f, paper, edge);
        const RECT ridge{rect.left + 3, rect.bottom - 3, rect.right - 3, rect.bottom - 2};
        Gdiplus::SolidBrush ridge_brush(Rgb(edge));
        graphics.FillRectangle(&ridge_brush, static_cast<float>(ridge.left), static_cast<float>(ridge.top), static_cast<float>(ridge.right - ridge.left), 1.f);
    }
}
void TextIn(HDC dc, const std::wstring& text, const RECT& rect, HFONT font, COLORREF color, UINT align = DT_CENTER) {
    const HGDIOBJ old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, color);
    RECT copy = rect;
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &copy, DT_SINGLELINE | DT_VCENTER | align | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, old);
}
int Measure(HDC dc, HFONT font, const std::wstring& text) {
    const HGDIOBJ old = SelectObject(dc, font);
    SIZE size{}; GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    SelectObject(dc, old);
    return static_cast<int>(size.cx);
}
bool ShowsText(Mark mark) {
    return mark == Mark::Key || mark == Mark::Disc || mark == Mark::Stick;
}
// The game's keyboard prompts are white keycaps with bold black captions
// (KeyboardMouse/keyboard_*.uasset); high contrast keeps the themed outline.
HFONT KeycapFont(int height) {
    static HFONT font = nullptr; static int cached = 0;
    if (font && cached == height) return font;
    if (font) DeleteObject(font);
    cached = height;
    font = CreateFontW(-(std::max)(8, height * 3 / 5), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    return font;
}
int KeycapWidth(HDC dc, int height, const std::wstring& caption) {
    return (std::max)(height, Measure(dc, KeycapFont(height), caption) + height * 11 / 20);
}
void Keycap(HDC dc, const RECT& rect, const std::wstring& caption) {
    const int height = rect.bottom - rect.top;
    if (UiTheme::HighContrast()) {
        Gdiplus::Graphics graphics(dc); graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Round(graphics, rect, height * .2f, UiTheme::Panel(), UiTheme::Text());
        TextIn(dc, caption, rect, KeycapFont(height), UiTheme::Text());
        return;
    }
    {
        Gdiplus::Graphics graphics(dc); graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Round(graphics, rect, (std::max)(2.f, height * .12f), RGB(0xee, 0xea, 0xe2), RGB(0xee, 0xea, 0xe2));
    }
    TextIn(dc, caption, rect, KeycapFont(height), RGB(0x10, 0x10, 0x10));
}
int PartWidth(HDC dc, const Part& part, int height) {
    if (part.asset >= 0) return UiAssets::WidthAt(static_cast<UiAssets::Id>(part.asset), height);
    if (part.mark == Mark::Key) return KeycapWidth(dc, height, part.text);
    return height;
}
// Xbox View has no game icon; draw it in the same white-disc style.
void ViewDisc(HDC dc, const RECT& rect) {
    Gdiplus::Graphics graphics(dc); graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const float x = static_cast<float>(rect.left), y = static_cast<float>(rect.top), d = static_cast<float>(rect.bottom - rect.top);
    Gdiplus::SolidBrush paper(Gdiplus::Color(255, 0xf4, 0xf4, 0xf4));
    graphics.FillEllipse(&paper, x + d * .04f, y + d * .04f, d * .92f, d * .92f);
    Gdiplus::Pen ink(Gdiplus::Color(255, 0x10, 0x10, 0x10), (std::max)(1.2f, d * .07f));
    graphics.DrawRectangle(&ink, x + d * .26f, y + d * .32f, d * .3f, d * .26f);
    graphics.DrawRectangle(&ink, x + d * .44f, y + d * .44f, d * .3f, d * .26f);
}
void RememberKeys() {
    for (int vk = 1; vk < 256; ++vk) key_was_[vk] = Bindable(vk) && Down(vk);
}
int Newest(int previous) {
    if (previous && Down(previous) && Bindable(previous)) return previous;
    int fallback = 0;
    for (int vk = 1; vk < 256; ++vk) {
        if (!Bindable(vk) || !Down(vk)) continue;
        if (!key_was_[vk]) return vk;
        if (!fallback) fallback = vk;
    }
    return fallback;
}
void InvalidateArm() { if (arm_.control) InvalidateRect(arm_.control, nullptr, FALSE); }
void Disarm() {
    const HWND control = arm_.control;
    arm_ = {};
    if (control) InvalidateRect(control, nullptr, FALSE);
}
bool Commit(Action action, const Chord& chord) {
    const int index = static_cast<int>(action);
    if (index < 0 || index >= kCount) return false;
    if (!chord.empty()) for (int i = 0; i < kCount; ++i) {
        if (i == index) continue;
        if (Conflicts(chord, chords_[static_cast<size_t>(i)])) {
            DebugHub::Notify(L"这个快捷键已经用于其他功能");
            return false;
        }
    }
    chords_[static_cast<size_t>(index)] = chord;
    Save(index);
    block_ = chord;
    blocking_ = !chord.empty();
    DebugHub::RefreshChrome();
    if (boxes_[static_cast<size_t>(index)]) InvalidateRect(boxes_[static_cast<size_t>(index)], nullptr, FALSE);
    return true;
}
void ArmAt(HWND control, int index) {
    if (index < 0 || index >= kCount) return;
    if (arm_.active && arm_.control == control) return;
    Disarm();
    arm_.active = true;
    arm_.control = control;
    arm_.action = static_cast<Action>(index);
    InvalidateArm();
}
void StepCapture() {
    const bool esc = Down(VK_ESCAPE), wipe = Down(VK_BACK) || Down(VK_DELETE);
    if (esc && !esc_was_) { Disarm(); esc_was_ = true; wipe_was_ = wipe; return; }
    if (wipe && !wipe_was_) {
        const Action action = arm_.action;
        Disarm();
        Commit(action, {});
        esc_was_ = esc; wipe_was_ = true;
        return;
    }
    esc_was_ = esc; wipe_was_ = wipe;
    if (!arm_.click_up) {
        if (!Down(VK_LBUTTON)) arm_.click_up = true;
    } else if (Down(VK_LBUTTON) && arm_.control) {
        POINT cursor{}; GetCursorPos(&cursor);
        RECT rect{}; GetWindowRect(arm_.control, &rect);
        if (!PtInRect(&rect, cursor)) { Disarm(); return; }
    }
    if (!UiHost::InGame()) { Disarm(); return; }
    const int primary = Newest(arm_.primary);
    const unsigned pad = ReadPad();
    if (!arm_.ready) {
        if (!primary && !pad) arm_.ready = true;
        InvalidateArm();
        return;
    }
    if (primary) {
        arm_.key_held = true;
        arm_.primary = primary;
        arm_.pad_held = false;
        arm_.pad_union = 0;
        arm_.live = {};
        arm_.live.device = Device::Keyboard;
        arm_.live.modifiers = Mods();
        arm_.live.key = static_cast<unsigned>(primary);
        arm_.live_valid = true;
        InvalidateArm();
        return;
    }
    if (arm_.key_held) {
        const Chord chord = arm_.live;
        const Action action = arm_.action;
        Disarm();
        Commit(action, chord);
        return;
    }
    if (pad) {
        arm_.pad_held = true;
        arm_.pad_union |= pad;
        arm_.live = {};
        arm_.live.device = pad_style_;
        arm_.live.buttons = arm_.pad_union;
        arm_.live_valid = true;
        InvalidateArm();
        return;
    }
    if (arm_.pad_held && arm_.pad_union) {
        const Chord chord = arm_.live;
        const Action action = arm_.action;
        Disarm();
        Commit(action, chord);
    }
}
void Snapshot(bool fire) {
    const unsigned mods = Mods(), pad = ReadPad();
    if (blocking_ && !Satisfied(block_, mods, pad)) blocking_ = false;
    for (int i = 0; i < kCount; ++i) {
        const bool down = Satisfied(chords_[static_cast<size_t>(i)], mods, pad);
        if (fire && down && !was_[static_cast<size_t>(i)] && !(blocking_ && Conflicts(block_, chords_[static_cast<size_t>(i)]))) {
            if (handlers_[static_cast<size_t>(i)]) handlers_[static_cast<size_t>(i)]();
        }
        was_[static_cast<size_t>(i)] = down;
    }
}
LRESULT CALLBACK BindProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    const int index = static_cast<int>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        const auto* created = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created->lpCreateParams));
        return DefWindowProcW(window, message, wparam, lparam);
    }
    case WM_ERASEBKGND: return 1;
    case WM_SETFOCUS: ArmAt(window, index); return 0;
    case WM_KILLFOCUS:
        if (arm_.active && arm_.control == window) Disarm();
        InvalidateRect(window, nullptr, FALSE); return 0;
    case WM_LBUTTONDOWN: SetFocus(window); return 0;
    case WM_SETCURSOR: SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE;
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_BUTTON;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const HDC target = BeginPaint(window, &paint);
        RECT client{}; GetClientRect(window, &client);
        HDC dc = CreateCompatibleDC(target);
        HBITMAP bitmap = dc ? CreateCompatibleBitmap(target, (std::max)(1L, client.right), (std::max)(1L, client.bottom)) : nullptr;
        const HGDIOBJ old = bitmap ? SelectObject(dc, bitmap) : nullptr;
        HDC draw = bitmap ? dc : target;
        const bool armed = arm_.active && arm_.control == window;
        const COLORREF paper = UiTheme::Mix(UiTheme::Base(), UiTheme::Panel(), 72);
        const COLORREF edge = armed ? UiTheme::Accent() : UiTheme::Mix(UiTheme::Border(), UiTheme::Base(), 15);
        // The off-screen bitmap starts black: paint the page colour first so the
        // anti-aliased corners blend into the page instead of a black square.
        FillRect(draw, &client, UiTheme::Background());
        {
            Gdiplus::Graphics graphics(draw);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
            RECT inner = client; inner.right -= 1; inner.bottom -= 1;
            Round(graphics, inner, static_cast<float>(Scale(window, 8)), paper, edge);
        }
        const Chord chord = armed && arm_.live_valid ? arm_.live : (index >= 0 && index < kCount ? chords_[static_cast<size_t>(index)] : Chord{});
        const int height = (std::max)(16, static_cast<int>(client.bottom - Scale(window, 8)));
        const int top = (client.bottom - height) / 2;
        if (armed && !arm_.live_valid) {
            RECT text{Scale(window, 8), 0, client.right - Scale(window, 8), client.bottom};
            TextIn(draw, UiI18n::Translate(L"等待按键"), text, UiTheme::GetFont(UiTheme::Font::Small, window), UiTheme::Accent());
        } else if (chord.empty()) {
            RECT text{Scale(window, 8), 0, client.right - Scale(window, 8), client.bottom};
            TextIn(draw, UiI18n::Translate(L"未设置"), text, UiTheme::GetFont(UiTheme::Font::Small, window), UiTheme::MutedText());
        } else {
            Draw(draw, Scale(window, 6), top, height, chord, client.right - Scale(window, 6));
        }
        if (bitmap) {
            BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
            SelectObject(dc, old); DeleteObject(bitmap);
        }
        if (dc) DeleteDC(dc);
        EndPaint(window, &paint);
        return 0;
    }
    default: break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
void LayoutPage(HWND window) {
    if (!window) return;
    RECT client{}; GetClientRect(window, &client);
    const int margin = Scale(window, 16), row = Scale(window, 40), box_w = Scale(window, 280), box_h = Scale(window, 32);
    const int top = Scale(window, 64);
    for (int i = 0; i < kCount; ++i) if (boxes_[static_cast<size_t>(i)]) {
        const int y = top + RowOf(i) * row + (row - box_h) / 2;
        const int x = (std::max)(margin + Scale(window, 180), static_cast<int>(client.right) - margin - box_w);
        SetWindowPos(boxes_[static_cast<size_t>(i)], nullptr, x, y, box_w, box_h, SWP_NOZORDER | SWP_NOACTIVATE);
    }
}
LRESULT CALLBACK PageProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_ERASEBKGND: {
        RECT client{}; GetClientRect(window, &client);
        FillRect(reinterpret_cast<HDC>(wparam), &client, UiTheme::Background());
        return 1;
    }
    case WM_SIZE: LayoutPage(window); InvalidateRect(window, nullptr, FALSE); return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(window, &paint);
        RECT client{}; GetClientRect(window, &client);
        FillRect(dc, &client, UiTheme::Background());
        SetBkMode(dc, TRANSPARENT);
        RECT title{Scale(window, 16), Scale(window, 8), client.right - Scale(window, 16), Scale(window, 32)};
        TextIn(dc, UiI18n::Translate(L"快捷键"), title, UiTheme::GetFont(UiTheme::Font::Strong, window), UiTheme::Text(), DT_LEFT);
        RECT hint{Scale(window, 16), Scale(window, 32), client.right - Scale(window, 16), Scale(window, 58)};
        const HGDIOBJ old = SelectObject(dc, UiTheme::GetFont(UiTheme::Font::Small, window));
        SetTextColor(dc, UiTheme::MutedText());
        DrawTextW(dc, UiI18n::Translate(L"点击按键框后按下组合，松开保存。Esc 取消，Backspace 清除。").c_str(), -1, &hint,
            DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, old);
        const int row = Scale(window, 40), top = Scale(window, 64);
        for (int i = 0; i < kCount; ++i) {
            const int at = RowOf(i);
            if (at < 0) continue;
            RECT label{Scale(window, 16), top + at * row, client.right - Scale(window, 300), top + (at + 1) * row};
            if (boxes_[static_cast<size_t>(i)]) {
                RECT box{}; GetWindowRect(boxes_[static_cast<size_t>(i)], &box);
                MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&box), 2);
                label.right = box.left - Scale(window, 12);
            }
            const HGDIOBJ font = SelectObject(dc, UiTheme::GetFont(UiTheme::Font::Body, window));
            SetTextColor(dc, UiTheme::Text());
            DrawTextW(dc, UiI18n::Translate(kSpecs[i].label).c_str(), -1, &label, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
            SelectObject(dc, font);
        }
        EndPaint(window, &paint);
        return 0;
    }
    case WM_DESTROY:
        if (page_ == window) page_ = nullptr;
        boxes_.fill(nullptr);
        if (arm_.control && !IsWindow(arm_.control)) Disarm();
        return 0;
    default: break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
bool RegisterClasses() {
    WNDCLASSEXW cls{sizeof(cls)};
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    const std::pair<const wchar_t*, WNDPROC> items[] = {{kPageClass, PageProc}, {kBindClass, BindProc}};
    for (const auto& item : items) {
        cls.lpszClassName = item.first;
        cls.lpfnWndProc = item.second;
        if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    }
    return true;
}
}
void NoteSonyReport(int kind, const unsigned char* data, int size) {
    unsigned buttons = 0;
    if (!ParseSony(kind, data, size, buttons)) return;
    snoop_buttons_.store(buttons, std::memory_order_relaxed);
    snoop_time_.store(GetTickCount64(), std::memory_order_relaxed);
    if (kind == 1) PublishTouch(kind, data, size, false);
}
void NoteDualSenseTouch(const unsigned char* data, int size) {
    PublishTouch(1, data, size, true);
    unsigned buttons = 0;
    if (!ParseSony(1, data, size, buttons)) return;
    snoop_buttons_.store(buttons, std::memory_order_relaxed);
    snoop_time_.store(GetTickCount64(), std::memory_order_relaxed);
}
void YieldSony(bool yield) { yield_sony_.store(yield, std::memory_order_relaxed); }
void Initialize(const wchar_t* config_file) {
    Ensure();
    config_ = config_file ? config_file : L"";
    if (config_.empty()) return;
    for (int i = 0; i < kCount; ++i) {
        if (Fixed(i)) continue;
        wchar_t buffer[160]{};
        GetPrivateProfileStringW(L"Hotkeys", kSpecs[i].id, L"*", buffer, 160, config_.c_str());
        if (!wcscmp(buffer, L"*")) continue;
        Chord parsed;
        if (Parse(buffer, parsed)) chords_[static_cast<size_t>(i)] = parsed;
    }
    for (int i = 0; i < kCount; ++i) for (int j = 0; j < i; ++j) {
        if (Fixed(i) || !Conflicts(chords_[static_cast<size_t>(i)], chords_[static_cast<size_t>(j)])) continue;
        chords_[static_cast<size_t>(i)] = {};
        Save(i);
    }
}
void Bind(Action action, std::function<void()> handler) {
    const int index = static_cast<int>(action);
    if (index >= 0 && index < kCount) handlers_[static_cast<size_t>(index)] = std::move(handler);
}
void EnsureSonyThread() {
    if (sony_thread_) return;
    sony_thread_ = CreateThread(nullptr, 0, SonyThread, nullptr, 0, nullptr);
}
void Tick() {
    Ensure();
    EnsureSonyThread();
    UiHost::Tick();
    RefreshDevices();
    if (arm_.active) StepCapture();
    const bool fire = !arm_.active && UiHost::InGame() && !TextEntry();
    Snapshot(fire);
    RememberKeys();
    if (!arm_.active) {
        esc_was_ = Down(VK_ESCAPE);
        wipe_was_ = Down(VK_BACK) || Down(VK_DELETE);
    }
}
bool Capturing() { return arm_.active; }
Chord Get(Action action) {
    Ensure();
    const int index = static_cast<int>(action);
    return index >= 0 && index < kCount ? chords_[static_cast<size_t>(index)] : Chord{};
}
bool Bound(Action action) { return !Get(action).empty(); }
std::wstring Plain(const Chord& chord) {
    if (chord.empty()) return UiI18n::Translate(L"未设置");
    std::wstring text;
    const wchar_t* join = SwipeChoice(chord) ? L" / " : L" + ";
    for (const Part& part : Parts(chord)) {
        std::wstring piece = part.text;
        if (piece.empty()) {
            if (part.mark == Mark::Cross) piece = L"Cross";
            else if (part.mark == Mark::Ring) piece = L"Circle";
            else if (part.mark == Mark::Square) piece = L"Square";
            else if (part.mark == Mark::Triangle) piece = L"Triangle";
        }
        if (piece.empty()) continue;
        if (!text.empty()) text += join;
        text += UiI18n::Translate(piece);
    }
    return text;
}
int Draw(HDC dc, int x, int y, int height, const Chord& chord, int limit_x) {
    if (!dc || height < 8 || x >= limit_x) return x;
    if (chord.empty()) {
        const std::wstring text = UiI18n::Translate(L"未设置");
        RECT rect{x, y, limit_x, y + height};
        TextIn(dc, text, rect, UiTheme::GetFont(UiTheme::Font::Small, static_cast<UINT>((std::max)(96, height * 96 / 22))), UiTheme::MutedText());
        return (std::min)(limit_x, x + Measure(dc, UiTheme::GetFont(UiTheme::Font::Small, static_cast<UINT>((std::max)(96, height * 96 / 22))), text));
    }
    HFONT created = CreateFontW(-(height * 3 / 5), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HFONT font = created ? created : UiTheme::GetFont(UiTheme::Font::Small, static_cast<UINT>((std::max)(96, height * 96 / 22)));
    const COLORREF paper = UiTheme::Mix(UiTheme::Base(), UiTheme::Panel(), 70);
    const COLORREF edge = UiTheme::Mix(UiTheme::Border(), UiTheme::Base(), 18);
    const std::vector<Part> parts = Parts(chord);
    bool first = true;
    for (const Part& part : parts) {
        const bool art = part.asset >= 0 || part.mark == Mark::Key || (part.mark == Mark::View && chord.device == Device::Xbox && !UiTheme::HighContrast());
        const int width = art ? PartWidth(dc, part, height)
            : ShowsText(part.mark) ? (std::max)(height, (part.text.empty() ? 0 : Measure(dc, font, part.text)) + height / 2) : height;
        const int gap = height / 5;
        const int plus = first ? 0 : gap + height / 3;
        if (x + plus + width > limit_x) break;
        if (!first) {
            RECT plus_rect{x, y, x + plus, y + height};
            TextIn(dc, SwipeChoice(chord) ? L"/" : L"+", plus_rect, font, UiTheme::MutedText());
            x += plus;
        }
        first = false;
        RECT rect{x, y, x + width, y + height};
        if (part.asset >= 0) {
            Gdiplus::Graphics graphics(dc);
            UiAssets::Draw(graphics, static_cast<UiAssets::Id>(part.asset), Gdiplus::RectF(static_cast<float>(rect.left), static_cast<float>(rect.top),
                static_cast<float>(width), static_cast<float>(height)), 1.f, part.mirror);
            x += width + gap;
            continue;
        }
        if (part.mark == Mark::Key) { Keycap(dc, rect, part.text); x += width + gap; continue; }
        if (art) { ViewDisc(dc, rect); x += width + gap; continue; }
        {
            Gdiplus::Graphics graphics(dc);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Glyph(graphics, rect, part, paper, UiTheme::Text(), edge);
        }
        if (ShowsText(part.mark) && !part.text.empty()) {
            const COLORREF color = part.mark == Mark::Disc ? RGB(255, 255, 255) : UiTheme::Text();
            TextIn(dc, part.text, rect, font, color);
        }
        x += width + gap;
    }
    if (created) DeleteObject(created);
    return x;
}
int DrawKey(HDC dc, int x, int y, int height, const wchar_t* caption) {
    if (!dc || !caption || height < 8) return x;
    const std::wstring text = caption;
    const int width = KeycapWidth(dc, height, text);
    Keycap(dc, {x, y, x + width, y + height}, text);
    return x + width;
}
HWND CreatePage(HWND parent) {
    if (!parent || !RegisterClasses()) return nullptr;
    if (page_ && IsWindow(page_)) return page_;
    Ensure();
    const int width = Scale(parent, 760);
    const int height = Scale(parent, 72) + Rows() * Scale(parent, 40) + Scale(parent, 20);
    page_ = CreateWindowExW(0, kPageClass, L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, width, height, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!page_) return nullptr;
    for (int i = 0; i < kCount; ++i) {
        if (Fixed(i)) continue;
        boxes_[static_cast<size_t>(i)] = CreateWindowExW(0, kBindClass, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            0, 0, 10, 10, page_, nullptr, GetModuleHandleW(nullptr), reinterpret_cast<LPVOID>(static_cast<INT_PTR>(i)));
    }
    DebugHub::Attach(page_, parent, width, height);
    LayoutPage(page_);
    return page_;
}
}
