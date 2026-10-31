#pragma once
#include <windows.h>

// UI worker only. Owns a noninteractive native blur plate immediately behind
// this process's PBZ Hub; never applies effects to the game or another process.
namespace UiGlass {
enum class Mode { OpaqueFallback, NativeBlurPlate };
struct Snapshot {
    bool applied = false, composition = false, high_contrast = false;
    bool transparency_allowed = true, native_blur_accepted = false, plate_visible = false;
    bool dwm_corners = false; // DWM rounds the Hub; no window region is set.
    Mode mode = Mode::OpaqueFallback;
    HRESULT backdrop_result = E_FAIL; // Native API receipt only; not pixel proof.
    BYTE animation_opacity = 255, content_opacity = 255;
    HWND plate = nullptr;
};
bool Apply(HWND hub);
// Pass an animation alpha, not the final alpha. The glass steady-state content
// alpha is composed here so a caller must not also set layered attributes.
bool SetOpacity(HWND hub, BYTE animation_opacity);
// After move/resize/show/DPI/z-order changes. No activation or foreground calls.
void Sync(HWND hub);
void Hide(HWND hub);
Snapshot GetSnapshot(HWND hub);
void Shutdown();
}
