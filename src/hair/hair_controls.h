#pragma once
#include <windows.h>
#include <cstdint>

// Only the version-verified worker constructs this. UI callbacks and CVar writes
// stay on that worker; Force Groom (F6) only posts a request to the game-thread
// hook in groom_force.cpp, which is the one place engine functions are called.
class HairControls {
public:
    explicit HairControls(uintptr_t image_base, bool create_panel = true, void (*open_boss_menu)() = nullptr);
    ~HairControls();
    void Tick();
    void Restore();
    DWORD Status() const;
    bool ToggleHair();
    bool ToggleBounds();
    bool ToggleForce();
    bool ToggleOutfit();
    bool PlayCredits();
    bool ToggleTriggers();
    HWND CreatePage(HWND parent);
private:
    static LRESULT CALLBACK WindowProc(HWND, UINT, WPARAM, LPARAM);
    void CreatePanel();
    void UpdatePanel();
    void Layout();
    void ShowPanel(bool show);
    bool Foreground() const;
    bool RestoreBounds();
    bool RestoreHair();
    uintptr_t base_;
    HWND panel_ = nullptr;
    HWND status_text_ = nullptr;
    HWND hair_button_ = nullptr;
    HWND bounds_button_ = nullptr;
    HWND force_button_ = nullptr;
    HWND force_text_ = nullptr;
    HWND outfit_button_ = nullptr;
    HWND outfit_text_ = nullptr;
    HWND triggers_button_ = nullptr;
    HWND triggers_text_ = nullptr;
    HWND restore_button_ = nullptr;
    HWND boss_button_ = nullptr;
    HWND mode_section_ = nullptr;
    HWND force_section_ = nullptr;
    HWND note_text_ = nullptr;
    void (*open_boss_menu_)() = nullptr;
    bool shown_ = true;
    bool key_down_[4]{};
    bool bounds_on_ = false;
    bool hair_saved_ = false;
    uintptr_t hair_pointer_ = 0;
    uint64_t hair_original_ = 0;
    uint64_t hair_written_ = 0;
    uintptr_t bounds_pointer_ = 0;
    uint64_t bounds_original_ = 0;
    int32_t shader_original_ = 0;
    ULONGLONG next_refresh_ = 0;
    wchar_t notice_[160] = L"F6: Force Groom  Shift+F6: Outfits  F8: Mesh/Groom  F9: AABB  Shift+F9: Triggers  F7: Panel";
};

namespace HairMemory {
bool Read(uintptr_t, void*, size_t);
bool Writable(uintptr_t, size_t);
bool Exchange64(uintptr_t, uint64_t expected, uint64_t desired);
bool Exchange32(uintptr_t, int32_t expected, int32_t desired);
bool Pair(uintptr_t, int32_t maximum, uint64_t& value);
}
