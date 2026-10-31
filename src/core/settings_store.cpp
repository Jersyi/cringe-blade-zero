#include "settings_store.h"
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <map>

namespace SettingsStore {
namespace {
SRWLOCK lock_ = SRWLOCK_INIT;
std::wstring path_;
std::map<std::wstring, std::wstring> cache_;
std::wstring Key(const wchar_t* section, const wchar_t* key) { return std::wstring(section) + L"\x1f" + key; }
}
void Initialize(const wchar_t* ini_path) {
    AcquireSRWLockExclusive(&lock_);
    path_ = ini_path ? ini_path : L""; cache_.clear();
    ReleaseSRWLockExclusive(&lock_);
}
std::wstring Get(const wchar_t* section, const wchar_t* key, const wchar_t* fallback) {
    AcquireSRWLockExclusive(&lock_);
    const std::wstring id = Key(section, key);
    auto found = cache_.find(id);
    std::wstring value;
    if (found != cache_.end()) value = found->second;
    else if (!path_.empty()) {
        wchar_t buffer[2048]{};
        GetPrivateProfileStringW(section, key, L"\x1e", buffer, 2048, path_.c_str());
        value = buffer;
        cache_[id] = value;
    } else value = L"\x1e";
    ReleaseSRWLockExclusive(&lock_);
    return value == L"\x1e" ? std::wstring(fallback ? fallback : L"") : value;
}
bool GetBool(const wchar_t* section, const wchar_t* key, bool fallback) {
    const std::wstring value = Get(section, key, fallback ? L"1" : L"0");
    return value == L"1" || !_wcsicmp(value.c_str(), L"true");
}
float GetFloat(const wchar_t* section, const wchar_t* key, float fallback) {
    const std::wstring value = Get(section, key, L"");
    if (value.empty()) return fallback;
    wchar_t* end = nullptr; const float parsed = std::wcstof(value.c_str(), &end);
    return end && end != value.c_str() && std::isfinite(parsed) ? parsed : fallback;
}
int GetInt(const wchar_t* section, const wchar_t* key, int fallback) {
    const std::wstring value = Get(section, key, L"");
    if (value.empty()) return fallback;
    wchar_t* end = nullptr; const long parsed = std::wcstol(value.c_str(), &end, 10);
    return end && end != value.c_str() ? static_cast<int>(parsed) : fallback;
}
void Put(const wchar_t* section, const wchar_t* key, const std::wstring& value) {
    AcquireSRWLockExclusive(&lock_);
    const std::wstring id = Key(section, key);
    auto found = cache_.find(id);
    const bool changed = found == cache_.end() || found->second != value;
    if (changed) {
        cache_[id] = value;
        if (!path_.empty()) WritePrivateProfileStringW(section, key, value.c_str(), path_.c_str());
    }
    ReleaseSRWLockExclusive(&lock_);
}
void PutBool(const wchar_t* section, const wchar_t* key, bool value) { Put(section, key, value ? L"1" : L"0"); }
void PutFloat(const wchar_t* section, const wchar_t* key, float value) {
    wchar_t text[64]{}; swprintf_s(text, L"%.3f", value); Put(section, key, text);
}
void PutInt(const wchar_t* section, const wchar_t* key, int value) { Put(section, key, std::to_wstring(value)); }
}
