#include "ui_glass.h"
#include "ui_host.h"
#include <dwmapi.h>
#include <algorithm>
#include <cwchar>
#include <memory>
#include <unordered_map>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "advapi32.lib")

namespace UiGlass {
namespace {
constexpr wchar_t kHub[] = L"PBZDebugHubWindow_v1";
constexpr wchar_t kPlate[] = L"PBZDebugGlassPlate_v1";
constexpr wchar_t kOwner[] = L"PBZ.UiGlass.Owner";
constexpr DWORD kCornerAttribute = 33;
constexpr int kRound = 2;
// GDI controls remain readable, while the independent plate's actual blurred
// desktop is visible through the otherwise opaque GDI surface.
constexpr BYTE kContentAlpha = 216;
// SetWindowCompositionAttribute has a documented dynamically loaded ABI. Its
// AccentPolicy attribute (19) and blur state (3) are an undocumented extension.
// The public DWM transient backdrop was pixel-tested here but becomes a solid
// inactive material on a NOACTIVATE plate; it must not be advertised as blur.
struct AccentPolicy { int state; DWORD flags, color, animation; };
struct CompositionData { int attribute; void* data; SIZE_T size; };
using SetCompositionAttribute = BOOL(WINAPI*)(HWND, const CompositionData*);
static_assert(sizeof(AccentPolicy) == 16 && sizeof(CompositionData) == 24);
struct Record {
    HWND hub = nullptr, plate = nullptr;
    DWORD thread = 0;
    bool added_layered = false, original_layered_attributes = false;
    COLORREF original_key = 0;
    BYTE original_alpha = 255;
    DWORD original_flags = 0;
    HRGN original_region = nullptr;
    bool had_region = false, region_written = false;
    int original_corner = 0;
    bool original_corner_known = false;
    int width = 0, height = 0, dpi = 0;
    Snapshot snapshot;
};
std::unordered_map<HWND, std::unique_ptr<Record>> records;
bool class_registered_here = false;
bool ValidHub(HWND window) {
    DWORD process = 0;
    wchar_t name[96]{};
    return window && IsWindow(window) && GetWindowThreadProcessId(window, &process) == GetCurrentThreadId() &&
        process == GetCurrentProcessId() && GetClassNameW(window, name, 96) && !wcscmp(name, kHub);
}
Record* Find(HWND hub) {
    const auto item = records.find(hub);
    if (item == records.end() || !ValidHub(hub) || GetPropW(hub, kOwner) != item->second.get()) return nullptr;
    return item->second.get();
}
bool TransparencyAllowed() {
    DWORD value = 1, bytes = sizeof(value);
    const LSTATUS result = RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"EnableTransparency",
        RRF_RT_REG_DWORD, nullptr, &value, &bytes);
    return result != ERROR_SUCCESS || value != 0;
}
LRESULT CALLBACK PlateProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(window, &paint);
        RECT rect{}; GetClientRect(window, &rect);
        // Black GDI pixels on an extended DWM frame have zero alpha, exposing
        // the compositor's backdrop instead of covering it with an opaque tint.
        FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
bool EnsureClass() {
    WNDCLASSEXW type{sizeof(type)};
    type.lpfnWndProc = PlateProc; type.hInstance = GetModuleHandleW(nullptr); type.lpszClassName = kPlate;
    if (RegisterClassExW(&type)) { class_registered_here = true; return true; }
    if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    WNDCLASSEXW existing{sizeof(existing)};
    return GetClassInfoExW(type.hInstance, kPlate, &existing) && existing.lpfnWndProc == PlateProc;
}
void SetRoundRegion(Record& record, int width, int height) {
    const int dpi = static_cast<int>(GetDpiForWindow(record.hub));
    if (record.width == width && record.height == height && record.dpi == dpi) return;
    record.width = width; record.height = height; record.dpi = dpi;
    if (IsZoomed(record.hub)) {
        SetWindowRgn(record.hub, nullptr, TRUE); record.region_written = true;
        return;
    }
    // Windows 11 rounds (and anti-aliases) the corners itself; the region is the fallback.
    if (record.snapshot.dwm_corners) return;
    const int diameter = MulDiv(16, (std::max)(96, dpi), 96);
    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, diameter, diameter);
    if (region) {
        if (SetWindowRgn(record.hub, region, TRUE)) record.region_written = true;
        else DeleteObject(region); // On success ownership belongs to Windows.
    }
}
void Configure(Record& record) {
    BOOL composition = FALSE;
    record.snapshot.composition = SUCCEEDED(DwmIsCompositionEnabled(&composition)) && composition;
    HIGHCONTRASTW contrast{sizeof(contrast)};
    record.snapshot.high_contrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
        (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
    record.snapshot.transparency_allowed = TransparencyAllowed();
    record.snapshot.mode = Mode::OpaqueFallback;
    record.snapshot.plate_visible = false;
    record.snapshot.native_blur_accepted = false;
    record.snapshot.backdrop_result = E_NOTIMPL;
    if (!record.snapshot.composition || record.snapshot.high_contrast || !record.snapshot.transparency_allowed || !EnsureClass()) return;
    record.plate = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        kPlate, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!record.plate) { record.snapshot.backdrop_result = HRESULT_FROM_WIN32(GetLastError()); return; }
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(record.plate, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DwmSetWindowAttribute(record.plate, kCornerAttribute, &kRound, sizeof(kRound));
    const MARGINS all{-1, -1, -1, -1};
    const HRESULT frame = DwmExtendFrameIntoClientArea(record.plate, &all);
    const auto native = reinterpret_cast<SetCompositionAttribute>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute"));
    AccentPolicy policy{3, 0, 0, 0};
    const CompositionData data{19, &policy, sizeof(policy)};
    SetLastError(ERROR_SUCCESS);
    const BOOL accepted = native && SUCCEEDED(frame) ? native(record.plate, &data) : FALSE;
    const DWORD error = GetLastError();
    record.snapshot.backdrop_result = FAILED(frame) ? frame : accepted ? S_OK : error ? HRESULT_FROM_WIN32(error) : E_NOTIMPL;
    record.snapshot.native_blur_accepted = accepted != FALSE;
    if (record.snapshot.native_blur_accepted) {
        SetWindowLongPtrW(record.plate, GWL_EXSTYLE, GetWindowLongPtrW(record.plate, GWL_EXSTYLE) | WS_EX_LAYERED);
        if (SetLayeredWindowAttributes(record.plate, 0, record.snapshot.animation_opacity, LWA_ALPHA)) record.snapshot.mode = Mode::NativeBlurPlate;
        else { record.snapshot.native_blur_accepted = false; record.snapshot.backdrop_result = HRESULT_FROM_WIN32(GetLastError()); }
    }
    if (record.snapshot.mode != Mode::NativeBlurPlate) { DestroyWindow(record.plate); record.plate = nullptr; }
    if (record.plate && !UiHost::Attach(record.plate)) {
        DestroyWindow(record.plate); record.plate = nullptr;
        record.snapshot.mode = Mode::OpaqueFallback;
        record.snapshot.native_blur_accepted = false;
    }
    record.snapshot.plate = record.plate;
}
void Restore(Record& record) {
    if (record.plate && IsWindow(record.plate)) { UiHost::Detach(record.plate); DestroyWindow(record.plate); }
    if (!ValidHub(record.hub) || GetPropW(record.hub, kOwner) != &record) return;
    if (record.region_written) {
        HRGN restore = nullptr;
        if (record.had_region && record.original_region) {
            restore = CreateRectRgn(0, 0, 0, 0);
            if (restore && CombineRgn(restore, record.original_region, nullptr, RGN_COPY) == ERROR) { DeleteObject(restore); restore = nullptr; }
        }
        if (!SetWindowRgn(record.hub, restore, TRUE) && restore) DeleteObject(restore);
    }
    if (record.original_layered_attributes) SetLayeredWindowAttributes(record.hub, record.original_key, record.original_alpha, record.original_flags);
    else if (record.added_layered) SetWindowLongPtrW(record.hub, GWL_EXSTYLE, GetWindowLongPtrW(record.hub, GWL_EXSTYLE) & ~static_cast<LONG_PTR>(WS_EX_LAYERED));
    if (record.original_corner_known) DwmSetWindowAttribute(record.hub, kCornerAttribute, &record.original_corner, sizeof(record.original_corner));
    RemovePropW(record.hub, kOwner);
}
} // namespace
bool Apply(HWND hub) {
    if (!ValidHub(hub)) return false;
    if (Record* existing = Find(hub)) {
        if (existing->plate) DestroyWindow(existing->plate);
        existing->plate = nullptr; existing->snapshot.plate = nullptr;
        Configure(*existing);
        return SetOpacity(hub, existing->snapshot.animation_opacity);
    }
    const auto stale = records.find(hub);
    if (stale != records.end()) {
        Restore(*stale->second); if (stale->second->original_region) DeleteObject(stale->second->original_region);
        records.erase(stale);
    }
    auto record = std::make_unique<Record>(); record->hub = hub; record->thread = GetCurrentThreadId();
    const LONG_PTR style = GetWindowLongPtrW(hub, GWL_EXSTYLE);
    record->added_layered = (style & WS_EX_LAYERED) == 0;
    record->original_layered_attributes = GetLayeredWindowAttributes(hub, &record->original_key, &record->original_alpha, &record->original_flags) != FALSE;
    record->original_region = CreateRectRgn(0, 0, 0, 0);
    if (record->original_region) record->had_region = GetWindowRgn(hub, record->original_region) != ERROR;
    record->original_corner_known = SUCCEEDED(DwmGetWindowAttribute(hub, kCornerAttribute, &record->original_corner, sizeof(record->original_corner)));
    if (!SetPropW(hub, kOwner, record.get())) { if (record->original_region) DeleteObject(record->original_region); return false; }
    if (record->added_layered) {
        SetLastError(ERROR_SUCCESS);
        if (!SetWindowLongPtrW(hub, GWL_EXSTYLE, style | WS_EX_LAYERED) && GetLastError() != ERROR_SUCCESS) {
            RemovePropW(hub, kOwner); if (record->original_region) DeleteObject(record->original_region); return false;
        }
    }
    record->snapshot.dwm_corners = SUCCEEDED(DwmSetWindowAttribute(hub, kCornerAttribute, &kRound, sizeof(kRound)));
    Configure(*record); record->snapshot.applied = true;
    records.emplace(hub, std::move(record));
    if (SetOpacity(hub, 255)) return true;
    const auto failed = records.find(hub);
    Restore(*failed->second); if (failed->second->original_region) DeleteObject(failed->second->original_region);
    records.erase(failed); return false;
}
bool SetOpacity(HWND hub, BYTE opacity) {
    Record* record = Find(hub); if (!record) return false;
    record->snapshot.animation_opacity = opacity;
    const unsigned steady = record->snapshot.mode == Mode::NativeBlurPlate ? kContentAlpha : 255u;
    const BYTE composed = static_cast<BYTE>((static_cast<unsigned>(opacity) * steady + 127u) / 255u);
    if (!SetLayeredWindowAttributes(hub, 0, composed, LWA_ALPHA)) return false;
    if (record->plate && !SetLayeredWindowAttributes(record->plate, 0, opacity, LWA_ALPHA)) return false;
    record->snapshot.content_opacity = composed;
    Sync(hub); return true;
}
void Sync(HWND hub) {
    Record* record = Find(hub); if (!record) return;
    RECT rect{}; if (!GetWindowRect(hub, &rect)) return;
    const int width = static_cast<int>(rect.right - rect.left), height = static_cast<int>(rect.bottom - rect.top);
    if (width <= 0 || height <= 0) return;
    SetRoundRegion(*record, width, height);
    if (!record->plate) return;
    if (!(GetWindowLongPtrW(record->plate, GWL_STYLE) & WS_CHILD) && !UiHost::Attach(record->plate)) {
        ShowWindow(record->plate, SW_HIDE);
        record->snapshot.plate_visible = false;
        return;
    }
    int x = rect.left, y = rect.top;
    if (GetWindowLongPtrW(record->plate, GWL_STYLE) & WS_CHILD) {
        const POINT origin = UiHost::ClientOrigin();
        x -= origin.x; y -= origin.y;
    }
    const bool visible = IsWindowVisible(hub) && !IsIconic(hub) && record->snapshot.animation_opacity != 0;
    const UINT flags = SWP_NOACTIVATE | SWP_NOOWNERZORDER | (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW);
    // The plate is a sibling behind the hub, both clipped to the game client.
    SetWindowPos(record->plate, hub, x, y, width, height, flags);
    record->snapshot.plate_visible = IsWindowVisible(record->plate) != FALSE;
}
void Hide(HWND hub) {
    Record* record = Find(hub); if (!record) return;
    if (record->plate) ShowWindow(record->plate, SW_HIDE);
    record->snapshot.plate_visible = false;
}
Snapshot GetSnapshot(HWND hub) { const Record* record = Find(hub); return record ? record->snapshot : Snapshot{}; }
void Shutdown() {
    for (auto& item : records) {
        Restore(*item.second);
        if (item.second->original_region) DeleteObject(item.second->original_region);
    }
    records.clear();
    if (class_registered_here && UnregisterClassW(kPlate, GetModuleHandleW(nullptr))) class_registered_here = false;
}
} // namespace UiGlass
