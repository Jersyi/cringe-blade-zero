#pragma once
#include <windows.h>
#include <string>
#include <string_view>

// UI worker only; localization never changes backend identifiers or requests.
namespace UiI18n {
enum class Language { SimplifiedChinese, English };
void Initialize(const wchar_t* config_file = nullptr);
Language GetLanguage();
void SetLanguage(Language language);
std::wstring Translate(std::wstring_view source);
void Bind(HWND control,const wchar_t* source);
void SetText(HWND control,const wchar_t* source);
LRESULT AddString(HWND control,UINT message,WPARAM parameter,const wchar_t* source);
void SetCell(HWND list,int row,int column,const wchar_t* source);
void Cue(HWND control,const wchar_t* source);
void Apply(HWND root);
void Shutdown();
}
