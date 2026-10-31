#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include "exports.generated.h"

int wmain(int argc, wchar_t** argv) {
    // --forward-only: a proxy without the debug-menu worker (triggers_only/version.dll) has no status export.
    const bool forward_only = argc == 3 && !wcscmp(argv[2], L"--forward-only");
    if (argc != 2 && !forward_only) { std::printf("usage: proxy_test.exe <absolute proxy DLL path> [--forward-only]\n"); return 2; }
    wchar_t system_path[MAX_PATH]{};
    if (!GetSystemDirectoryW(system_path, MAX_PATH)) return 3;
    wcscat_s(system_path, L"\\version.dll");
    HMODULE real = LoadLibraryExW(system_path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE proxy = LoadLibraryW(argv[1]);
    if (!real || !proxy) { std::printf("load error %lu\n", GetLastError()); return 4; }
    for (unsigned int i = 0; i < kExportCount; ++i) {
        // The system DLL's ordinal sequence is 1..17 on this machine.
        if (!GetProcAddress(proxy, kExportNames[i]) || !GetProcAddress(proxy, MAKEINTRESOURCEA(i + 1))) return 5;
    }
    using SizeW = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
    using GetW = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID);
    using QueryW = BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
    const auto r_size = reinterpret_cast<SizeW>(GetProcAddress(real, "GetFileVersionInfoSizeW"));
    const auto p_size = reinterpret_cast<SizeW>(GetProcAddress(proxy, "GetFileVersionInfoSizeW"));
    const auto r_get = reinterpret_cast<GetW>(GetProcAddress(real, "GetFileVersionInfoW"));
    const auto p_get = reinterpret_cast<GetW>(GetProcAddress(proxy, "GetFileVersionInfoW"));
    const auto p_query = reinterpret_cast<QueryW>(GetProcAddress(proxy, "VerQueryValueW"));
    std::atomic<int> errors{0}; std::vector<std::thread> threads;
    // Concurrent first calls exercise lazy system resolution and x64 stack args.
    for (int i = 0; i < 8; ++i) threads.emplace_back([&] {
        for (int n = 0; n < 200; ++n) {
            DWORD a = 99, b = 88;
            const DWORD rn = r_size(system_path, &a), pn = p_size(system_path, &b);
            if (!rn || rn != pn || a != b) { ++errors; return; }
            std::vector<unsigned char> ra(rn), pa(pn);
            if (r_get(system_path, 0, rn, ra.data()) != p_get(system_path, 0, pn, pa.data()) || ra != pa) { ++errors; return; }
            void* value = nullptr; UINT size = 0;
            if (!p_query(pa.data(), L"\\", &value, &size) || !value || !size) { ++errors; return; }
        }
    });
    for (auto& t : threads) t.join();
    if (forward_only) {
        const bool absent = !GetProcAddress(proxy, "PBZDebugMenuStatus");
        if (!absent) ++errors;
        std::printf("exports=%zu forward_threads=8 forward_iterations=1600 errors=%d debug_menu_export=%s\n",
            kExportCount, errors.load(), absent ? "absent" : "PRESENT");
        return errors.load() ? 7 : 0;
    }
    using Status = DWORD(WINAPI*)();
    const auto status = reinterpret_cast<Status>(GetProcAddress(proxy, "PBZDebugMenuStatus"));
    if (!status) return 6;
    for (int n = 0; n < 100 && status() < 0x100; ++n) Sleep(10);
    const DWORD state = status();
    wchar_t host[MAX_PATH]{}; GetModuleFileNameW(nullptr, host, MAX_PATH);
    const wchar_t* name = wcsrchr(host, L'\\'); name = name ? name + 1 : host;
    const DWORD expected = _wcsicmp(name, L"PBZ.exe") ? 0x100 : 0x200;
    if (state != expected) ++errors;
    std::printf("exports=%zu forward_threads=8 forward_iterations=1600 errors=%d worker_status=0x%lx expected=0x%lx\n",
        kExportCount, errors.load(), state, expected);
    return errors.load() ? 7 : 0;
}
