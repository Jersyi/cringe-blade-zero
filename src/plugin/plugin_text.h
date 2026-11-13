#pragma once
// Small text and struct helpers shared by the plugin host (combo_plugins.cpp), the module registry and the plugin UI model.
// The plugin API is UTF-8 and C structs whose first field is their size (sdk/pbz_combo_api.h).
#include "pbz_combo_api.h"
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace PluginText {
constexpr size_t kTextMax = 800;

inline std::wstring Wide(const char* text, size_t most = kTextMax) {
    if (!text || !*text) return {};
    const size_t length = strnlen(text, most);
    const int count = MultiByteToWideChar(CP_UTF8, 0, text, static_cast<int>(length), nullptr, 0);
    if (count <= 0) return {};
    std::wstring out(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, static_cast<int>(length), out.data(), count);
    return out;
}
inline std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string out(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), count, nullptr, nullptr);
    return out;
}
inline std::string Short(const char* text, size_t most) { return text ? std::string(text, strnlen(text, most)) : std::string(); }
// A struct the plugin passed: its first `size` bytes over a zeroed T (fields it does not know stay 0).
template <class T> bool ReadIn(const T* in, T& out) {
    if (!in || in->size < sizeof(uint32_t)) return false;
    out = T{}; std::memcpy(&out, in, (std::min)(static_cast<size_t>(in->size), sizeof(T))); out.size = static_cast<uint32_t>(sizeof(T));
    return true;
}
// A struct for the plugin: filled whole here, then the plugin's `size` bytes of it written back.
template <class T> bool WriteOut(T* out, const T& value) {
    if (!out || out->size < sizeof(uint32_t)) return false;
    const uint32_t size = out->size; std::memcpy(out, &value, (std::min)(static_cast<size_t>(size), sizeof(T))); out->size = size;
    return true;
}
template <size_t N> void CopyText(char (&to)[N], const std::string& from) { strncpy_s(to, N, from.c_str(), _TRUNCATE); }
// Writes text and its terminating 0 into the plugin's buffer; `needed` is the size that holds it (PBZ_E_BUFFER = -7 when it does not fit).
inline PbzResult CopyOut(const std::string& text, char* buffer, int32_t size, int32_t* needed) {
    const int32_t want = static_cast<int32_t>((std::min)(text.size(), static_cast<size_t>(INT32_MAX - 1)) + 1);
    if (needed) *needed = want;
    if (!buffer || size < want) { if (buffer && size > 0) buffer[0] = 0; return PBZ_E_BUFFER; }
    std::memcpy(buffer, text.data(), static_cast<size_t>(want - 1)); buffer[want - 1] = 0;
    return PBZ_OK;
}
// Table cells: a tab or a line break would end the cell or the row.
inline std::string Cell(const std::string& text) {
    std::string out = text;
    for (char& c : out) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return out;
}
// "name" parts of ids: letters, digits, '_', '-', '.', 1..max bytes.
inline bool IdText(const std::string& text, size_t most = 48) {
    if (text.empty() || text.size() > most) return false;
    for (const unsigned char c : text) if (!(std::isalnum(c) || c == '_' || c == '-' || c == '.')) return false;
    return true;
}
// The words of a command argument ("a b  c" -> a, b, c).
inline std::vector<std::string> Words(const std::string& text) {
    std::vector<std::string> out; std::istringstream in(text); std::string word;
    while (in >> word) out.push_back(word);
    return out;
}
// A whole number or a finite decimal from text; false when it is neither.
inline bool ToInt(const std::string& text, int& out) {
    char* end = nullptr; const long v = std::strtol(text.c_str(), &end, 10);
    if (text.empty() || !end || *end || v < INT32_MIN || v > INT32_MAX) return false;
    out = static_cast<int>(v); return true;
}
inline bool ToDouble(const std::string& text, double& out) {
    char* end = nullptr; const double v = std::strtod(text.c_str(), &end);
    if (text.empty() || !end || *end || !(v == v) || v > 1e300 || v < -1e300) return false;
    out = v; return true;
}
}  // namespace PluginText
