#pragma once
#include <string>

// PBZDebugMenu.ini persistence shared by the worker and the game thread.
// Values are cached; Put writes the file only when a value actually changes,
// so callers may save on every state change without extra I/O.
namespace SettingsStore {
void Initialize(const wchar_t* ini_path);
std::wstring Get(const wchar_t* section, const wchar_t* key, const wchar_t* fallback = L"");
bool GetBool(const wchar_t* section, const wchar_t* key, bool fallback);
float GetFloat(const wchar_t* section, const wchar_t* key, float fallback);
int GetInt(const wchar_t* section, const wchar_t* key, int fallback);
void Put(const wchar_t* section, const wchar_t* key, const std::wstring& value);
void PutBool(const wchar_t* section, const wchar_t* key, bool value);
void PutFloat(const wchar_t* section, const wchar_t* key, float value);
void PutInt(const wchar_t* section, const wchar_t* key, int value);
}
