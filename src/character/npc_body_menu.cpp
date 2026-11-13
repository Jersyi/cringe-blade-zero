// 身材 / 骨骼 页（2026-11-13 起代替「骨骼变换」页）：F10 面板里的常驻入口。
//
// 编辑控件全在搬进来的 UUEPBS 编辑器那边（NpcBridge，独立窗口，CPU 渲染）；这一页只有总开关、打开按钮和状态，
// 免得两套界面对同一份数据各画一遍。早期自写的简化页 / 详细页 / 预设页已删除（合并时它们早已不注册）。
#include "npc_body_menu.h"

#include "debug_hub.h"
#include "npc_body.h"
#include "npc_bridge.h"

#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <string>

namespace NpcBodyMenu
{
    namespace
    {
        constexpr int kLauncherButton = 9401, kEnableBox = 9402;
        Logger logger_ = nullptr;
        DWORD worker_thread_ = 0;
        HWND launcher_ = nullptr;
        HWND enable_box_ = nullptr;
        HWND launcher_button_ = nullptr;
        HWND launcher_status_ = nullptr;
        HFONT launcher_font_ = nullptr;
        ULONGLONG next_refresh_ = 0;
        std::wstring shown_status_;
        volatile LONG show_requested_ = 0;
        const wchar_t* const kLauncherClass = L"PBZ_NpcBody_Launcher_v2";

        void Log(const char* format, ...)
        {
            if (!logger_) return;
            char text[512]{};
            va_list args;
            va_start(args, format);
            _vsnprintf_s(text, sizeof(text), _TRUNCATE, format, args);
            va_end(args);
            logger_(text);
        }

        std::wstring Wide(const char* text)
        {
            if (!text || !*text) return std::wstring();
            const int need = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
            if (need <= 1) return std::wstring();
            std::wstring out(static_cast<size_t>(need), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), need);
            out.resize(static_cast<size_t>(need) - 1);
            return out;
        }

        std::wstring StatusText()
        {
            const bool on = NpcBridge::Enabled();
            std::wstring text = on ? (NpcBridge::Visible() ? L"编辑器：已打开（再点一次关闭，F11 也可以）"
                                                           : L"编辑器：未打开（点上面的按钮，或按 F11）")
                                   : L"身材 / 骨骼：关闭（主角的身材不做任何修改）。打开编辑器即启用。";
            if (on)
            {
                text += L"\r\n骨骼钩子：";
                text += NpcBody::Hooked() ? L"已装上" : L"未装上";
                const std::wstring note = Wide(NpcBody::HookReport());
                if (!note.empty()) text += L"（" + note + L"）";
                text += L"\r\n变身成 BOSS 期间编辑暂停，变回主角后继续。预设与动画在游戏目录 platform\\<商店>\\UUEPBS Presets。";
            }
            return text;
        }

        void RefreshLauncher(bool force)
        {
            if (!launcher_status_) return;
            const std::wstring text = StatusText();
            if (force || text != shown_status_)
            {
                shown_status_ = text;
                SetWindowTextW(launcher_status_, text.c_str());
            }
            if (enable_box_) SendMessageW(enable_box_, BM_SETCHECK, NpcBridge::Enabled() ? BST_CHECKED : BST_UNCHECKED, 0);
        }

        LRESULT CALLBACK LauncherProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
        {
            switch (message)
            {
            case WM_SIZE:
            {
                RECT client{};
                GetClientRect(window, &client);
                const int pad = 16, high = 30, gap = 10;
                int y = pad;
                if (enable_box_) MoveWindow(enable_box_, pad, y, client.right - pad * 2, high, TRUE);
                y += high + gap;
                if (launcher_button_) MoveWindow(launcher_button_, pad, y, client.right - pad * 2, 34, TRUE);
                y += 34 + gap + 4;
                if (launcher_status_) MoveWindow(launcher_status_, pad, y, client.right - pad * 2, (std::max)(80, static_cast<int>(client.bottom) - y - pad), TRUE);
                return 0;
            }
            case WM_COMMAND:
                if (LOWORD(wparam) == kLauncherButton)
                {
                    NpcBridge::Toggle();
                    Log("BODY_MENU editor %s from the page", NpcBridge::Visible() ? "hidden" : "shown");
                    RefreshLauncher(true);
                }
                else if (LOWORD(wparam) == kEnableBox && HIWORD(wparam) == BN_CLICKED)
                {
                    const bool on = SendMessageW(enable_box_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    NpcBridge::SetEnabled(on);
                    Log("BODY_MENU [NpcBody] Enabled=%d from the page", on ? 1 : 0);
                    RefreshLauncher(true);
                }
                return 0;
            case WM_CTLCOLORSTATIC:
            case WM_CTLCOLORBTN:
            {
                HDC dc = reinterpret_cast<HDC>(wparam);
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, RGB(206, 206, 206));
                return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
            }
            default:
                break;
            }
            return DefWindowProcW(window, message, wparam, lparam);
        }

        HWND MakeLauncher(HWND parent)
        {
            if (!parent) return nullptr;
            if (!launcher_)
            {
                WNDCLASSEXW type{};
                type.cbSize = sizeof(type);
                type.lpfnWndProc = LauncherProcedure;
                type.hInstance = GetModuleHandleW(nullptr);
                type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
                type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
                type.lpszClassName = kLauncherClass;
                if (!RegisterClassExW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
                {
                    Log("BODY_MENU launcher class registration failed");
                    return nullptr;
                }
                launcher_ = CreateWindowExW(0, kLauncherClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, parent, nullptr, type.hInstance, nullptr);
                if (!launcher_) return nullptr;
                NONCLIENTMETRICSW metrics{};
                metrics.cbSize = sizeof(metrics);
                if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
                {
                    LOGFONTW font = metrics.lfMessageFont;
                    font.lfHeight = -11;
                    launcher_font_ = CreateFontIndirectW(&font);
                }
                enable_box_ = CreateWindowExW(0, L"BUTTON", L"启用身材 / 骨骼（UUEPBS 编辑器：部位滑条、逐骨调整、预设、动画）",
                                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, launcher_,
                                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEnableBox)), type.hInstance, nullptr);
                launcher_button_ = CreateWindowExW(0, L"BUTTON", L"打开 / 关闭 身材编辑器",
                                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, launcher_,
                                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(kLauncherButton)), type.hInstance, nullptr);
                launcher_status_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, launcher_, nullptr,
                                                   type.hInstance, nullptr);
                if (launcher_font_)
                    for (HWND control : {enable_box_, launcher_button_, launcher_status_})
                        if (control) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(launcher_font_), TRUE);
            }
            else if (GetParent(launcher_) != parent)
            {
                SetParent(launcher_, parent);
            }
            DebugHub::Attach(launcher_, parent, 520, 220);
            RefreshLauncher(true);
            return launcher_;
        }
    }  // namespace

    void Initialize(Logger logger)
    {
        logger_ = logger;
        worker_thread_ = GetCurrentThreadId();
    }

    HWND CreateLauncherPage(HWND parent)
    {
        if (worker_thread_ && worker_thread_ != GetCurrentThreadId()) return nullptr;
        return MakeLauncher(parent);
    }

    void Show()
    {
        if (DebugHub::Enabled())
        {
            DebugHub::ShowPage(DebugHub::Page::BoneMotion);
            return;
        }
        InterlockedExchange(&show_requested_, 1);
    }

    void Tick()
    {
        if (!worker_thread_ || worker_thread_ != GetCurrentThreadId()) return;
        if (InterlockedExchange(&show_requested_, 0)) NpcBridge::Toggle();  // no panel: the editor itself
        if (!launcher_ || !IsWindowVisible(launcher_)) return;
        const ULONGLONG now = GetTickCount64();
        if (now < next_refresh_) return;
        next_refresh_ = now + 400;
        RefreshLauncher(false);
    }

    void Shutdown()
    {
        if (!worker_thread_ || worker_thread_ != GetCurrentThreadId()) return;
        if (launcher_) DestroyWindow(launcher_);
        launcher_ = enable_box_ = launcher_button_ = launcher_status_ = nullptr;
        if (launcher_font_)
        {
            DeleteObject(launcher_font_);
            launcher_font_ = nullptr;
        }
        UnregisterClassW(kLauncherClass, GetModuleHandleW(nullptr));
        NpcBridge::Stop();
        worker_thread_ = 0;
    }
}  // namespace NpcBodyMenu
