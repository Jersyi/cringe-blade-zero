// Standalone version.dll proxy: DualSense adaptive triggers always off (../pad_triggers.h), nothing else.
// No debug menu, no panel, no hotkeys, no settings file, no game-memory writes; only a short log next to the DLL.
#include <windows.h>
#include <array>
#include <cstdio>
#include <cwchar>
#include "exports.generated.h"
#include "pad_triggers.h"

namespace {
HMODULE self_module = nullptr;
INIT_ONCE system_once = INIT_ONCE_STATIC_INIT;
std::array<FARPROC, kExportCount> system_functions{};
wchar_t log_path[MAX_PATH]{};

void Log(const char* message) {
    if (!log_path[0]) return;
    SYSTEMTIME t{}; GetLocalTime(&t);
    char line[640]{};
    const int n = _snprintf_s(line, sizeof(line), _TRUNCATE, "%04u-%02u-%02u %02u:%02u:%02u pid=%lu %s\r\n",
        t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, GetCurrentProcessId(), message);
    if (n <= 0) return;
    HANDLE file = CreateFileW(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) && size.QuadPart >= 256 * 1024) {  // keep one previous log
        CloseHandle(file);
        wchar_t previous[MAX_PATH + 16]{}; swprintf_s(previous, L"%s.previous", log_path);
        if (!MoveFileExW(log_path, previous, MOVEFILE_REPLACE_EXISTING)) return;
        file = CreateFileW(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
    }
    DWORD written = 0; WriteFile(file, line, static_cast<DWORD>(n), &written, nullptr); CloseHandle(file);
}

ULONG_PTR WINAPI MissingExport() {
    SetLastError(ERROR_PROC_NOT_FOUND);
    return 0;
}
BOOL CALLBACK LoadSystem(PINIT_ONCE, PVOID, PVOID*) {
    wchar_t directory[MAX_PATH]{};
    const UINT n = GetSystemDirectoryW(directory, MAX_PATH);
    if (n > 0 && n + 13 < MAX_PATH) {
        wcscat_s(directory, L"\\version.dll");
        HMODULE real = LoadLibraryExW(directory, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (real && real != self_module)
            for (size_t i = 0; i < kExportCount; ++i) system_functions[i] = GetProcAddress(real, kExportNames[i]);
    }
    for (auto& fn : system_functions)
        if (!fn) fn = reinterpret_cast<FARPROC>(&MissingExport);
    return TRUE;
}

DWORD WINAPI Worker(void*) {
    // The hooks never unpatch, so their code must stay mapped for the life of the process.
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&Worker), &pinned);
    if (GetModuleFileNameW(self_module, log_path, MAX_PATH)) {
        if (wchar_t* slash = wcsrchr(log_path, L'\\')) wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - log_path), L"PBZTriggersOff.log");
        else log_path[0] = 0;
    }
    PadTriggers::Attach(&Log, nullptr);
    return 0;
}
}  // namespace

extern "C" FARPROC ResolveVersionExport(unsigned int index) {
    const DWORD saved_error = GetLastError();
    InitOnceExecuteOnce(&system_once, LoadSystem, nullptr, nullptr);
    FARPROC answer = index < system_functions.size() ? system_functions[index] : reinterpret_cast<FARPROC>(&MissingExport);
    SetLastError(saved_error);
    return answer;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        self_module = module;
        // Import-table writes of the host executable only, before its code runs; the Steam watcher and log start on
        // a thread, which runs once the loader is done.
        PadTriggers::Install();
        if (HANDLE thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr)) CloseHandle(thread);
    }
    return TRUE;
}
