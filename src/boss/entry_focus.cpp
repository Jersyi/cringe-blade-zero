#include "entry_focus.h"
#include "debug_hub.h"
#include <windows.h>
#include <atomic>
#include <cwchar>

namespace EntryFocus {
namespace {
constexpr unsigned char kMenu = 1, kGameplay = 2;
static_assert(std::atomic<unsigned char>::is_always_lock_free, "GT focus getters must not acquire an OS lock");
std::atomic<unsigned char> focus_{0};
std::atomic<DWORD> process_id_{0};

// Exact registered class names, read from the current implementation files.
// Do not classify windows by title, since titles change with localization.
constexpr const wchar_t* kMenus[] = {
    L"PBZDebugHubWindow_v1",       // Unified worker-thread tabbed host.
    L"PBZBossRushMenu_v1",          // boss_menu.cpp: F10 Boss Rush window.
    L"PBZCharacterSwap_v1",         // boss_menu.cpp: native outfit window.
    L"PBZ.DebugDLL.Boss.AdvancedAI", // boss_ai_menu.cpp.
    L"PBZ_NpcRetarget_Window_v1",    // npc_menu.cpp.
    L"PBZHairControls_v3",          // hair_controls.cpp.
    L"PBZ_AirSkills_JC_Window_v1",   // air_skills_menu.cpp.
    L"PBZMotianGrabWindow",         // motian_grab_menu.cpp.
};
// FWindowsWindow::AppWindowClass, WindowsWindow.cpp:38 in the local UE source.
constexpr const wchar_t* kUnrealClass = L"UnrealWindow";

bool BelongsToProcess(HWND window, DWORD process) {
    DWORD owner = 0;
    return window && GetWindowThreadProcessId(window, &owner) != 0 && owner == process;
}
bool IsMenu(HWND window) {
    wchar_t name[128]{};
    if (!window || GetClassNameW(window, name, static_cast<int>(sizeof(name) / sizeof(name[0]))) <= 0) return false;
    for (const wchar_t* menu : kMenus) if (!_wcsicmp(name, menu)) return true;
    return false;
}
bool IsGameplay(HWND window) {
    wchar_t name[128]{};
    return window && !IsIconic(window) && IsWindowVisible(window) &&
        GetClassNameW(window, name, static_cast<int>(sizeof(name) / sizeof(name[0]))) > 0 && !_wcsicmp(name, kUnrealClass);
}
bool FocusChainMenu(DWORD process) {
    // A hub parented to UnrealWindow never becomes the foreground window.
    // Walk this thread's focus parents so the open menu still counts.
    HWND window = GetFocus();
    HWND visited[16]{};
    for (unsigned int i = 0; window && i < sizeof(visited) / sizeof(visited[0]); ++i) {
        if (!BelongsToProcess(window, process)) return false;
        for (unsigned int j = 0; j < i; ++j) if (visited[j] == window) return false;
        visited[i] = window;
        if (IsMenu(window)) return true;
        const HWND parent = GetParent(window);
        window = parent ? parent : GetWindow(window, GW_OWNER);
    }
    return false;
}
bool MenuOrOwnedDialog(HWND foreground, DWORD process) {
    // A child edit/combo control, a popup and a MessageBox owned by one of the
    // menus should all keep the ready encounter paused. Ownership is traversed
    // only inside our process and is bounded to tolerate transient window teardown.
    HWND window = foreground;
    HWND visited[16]{};
    for (unsigned int i = 0; window && i < sizeof(visited) / sizeof(visited[0]); ++i) {
        if (!BelongsToProcess(window, process)) return false;
        for (unsigned int j = 0; j < i; ++j) if (visited[j] == window) return false;
        visited[i] = window;
        if (IsMenu(window)) return true;
        const HWND root = GetAncestor(window, GA_ROOT);
        if (root && root != window) { window = root; continue; }
        window = GetWindow(window, GW_OWNER);
    }
    return false;
}
}

void Initialize() {
    focus_.store(0, std::memory_order_release);
    process_id_.store(GetCurrentProcessId(), std::memory_order_release);
    Poll();
}
void Poll() {
    const DWORD process = process_id_.load(std::memory_order_acquire);
    const HWND foreground = GetForegroundWindow();
    unsigned char next = 0;
    if (process && (FocusChainMenu(process) || DebugHub::ObscuresGameplay() ||
        (BelongsToProcess(foreground, process) && MenuOrOwnedDialog(foreground, process)))) next = kMenu;
    else if (process && BelongsToProcess(foreground, process)) {
        // Do not follow GW_OWNER here: a native modal dialog may be owned
        // by an Unreal window, but it is not an available gameplay surface.
        const HWND root = GetAncestor(foreground, GA_ROOT);
        if (BelongsToProcess(root, process) && IsGameplay(root)) next = kGameplay;
    }
    if (GetForegroundWindow() != foreground) next = 0;
    focus_.store(next, std::memory_order_release);
}
bool MenuFocused() noexcept { return (focus_.load(std::memory_order_acquire) & kMenu) != 0; }
bool GameplayFocused() noexcept { return (focus_.load(std::memory_order_acquire) & kGameplay) != 0; }
}
