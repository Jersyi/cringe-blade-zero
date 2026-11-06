#include "combo_file.h"
#include <windows.h>
#include <compressapi.h>
#include <cstring>

namespace ComboFile {
namespace {
constexpr char kMagic[8] = {'P', 'B', 'Z', 'C', 'O', 'M', 'B', 'O'};
constexpr char kFirstLine[] = "PBZCOMBO 1";
enum : uint16_t { kStored = 0, kXpressHuff = 1 };
void Put16(std::vector<uint8_t>& out, uint16_t v) { out.push_back(static_cast<uint8_t>(v)); out.push_back(static_cast<uint8_t>(v >> 8)); }
void Put32(std::vector<uint8_t>& out, uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i))); }
uint16_t Get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t Get32(const uint8_t* p) { return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24); }
std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size > 0 ? size : 0), '\0');
    if (size > 0) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}
bool Wide(const std::string& text, std::wstring& out) {
    out.clear();
    if (text.empty()) return true;
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return false;
    out.assign(static_cast<size_t>(size), L'\0');
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), static_cast<int>(text.size()), out.data(), size) == size;
}
bool XpressPack(const std::string& in, std::vector<uint8_t>& out) {
    COMPRESSOR_HANDLE handle = nullptr;
    if (!CreateCompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &handle)) return false;
    SIZE_T need = 0;
    bool ok = false;
    if (!Compress(handle, in.data(), in.size(), nullptr, 0, &need) && GetLastError() == ERROR_INSUFFICIENT_BUFFER && need) {
        out.resize(need);
        SIZE_T used = 0;
        ok = Compress(handle, in.data(), in.size(), out.data(), out.size(), &used) != FALSE;
        out.resize(ok ? used : 0);
    }
    CloseCompressor(handle);
    return ok;
}
bool XpressUnpack(const uint8_t* in, size_t size, size_t raw, std::string& out) {
    DECOMPRESSOR_HANDLE handle = nullptr;
    if (!CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &handle)) return false;
    out.assign(raw, '\0');
    SIZE_T used = 0;
    const bool ok = Decompress(handle, in, size, out.data(), out.size(), &used) && used == raw;
    CloseDecompressor(handle);
    return ok;
}
}
uint32_t Crc32(const void* data, size_t size) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}
bool ProfileName(const std::string& profile) {
    if (profile.empty() || profile.size() > 64) return false;
    for (const char c : profile) if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}
std::vector<uint8_t> Pack(const Set& set, bool compress) {
    std::string payload = std::string(kFirstLine) + "\n";
    for (const auto& [profile, text] : set) {
        if (!ProfileName(profile)) continue;
        std::string line = Utf8(text);
        if (line.find_first_of("\t\r\n") != std::string::npos) continue;
        payload += profile + "\t" + line + "\n";
    }
    std::vector<uint8_t> packed;
    uint16_t method = kStored;
    if (compress && XpressPack(payload, packed) && packed.size() < payload.size()) method = kXpressHuff;
    else packed.assign(payload.begin(), payload.end());
    std::vector<uint8_t> out(kMagic, kMagic + sizeof(kMagic));
    Put16(out, kVersion); Put16(out, method);
    Put32(out, static_cast<uint32_t>(payload.size())); Put32(out, Crc32(payload.data(), payload.size())); Put32(out, static_cast<uint32_t>(packed.size()));
    out.insert(out.end(), packed.begin(), packed.end());
    return out;
}
bool Unpack(const std::vector<uint8_t>& bytes, Set& set, std::string& why) {
    set.clear();
    if (bytes.size() < kHeader || std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) != 0) { why = "not a .combo file (magic)"; return false; }
    const uint8_t* p = bytes.data();
    const uint16_t version = Get16(p + 8), method = Get16(p + 10);
    const uint32_t raw = Get32(p + 12), crc = Get32(p + 16), packed = Get32(p + 20);
    if (version != kVersion) { why = "unsupported version " + std::to_string(version); return false; }
    if (raw > kMaxPayload || packed != bytes.size() - kHeader) { why = "bad sizes"; return false; }
    std::string payload;
    if (method == kStored) {
        if (packed != raw) { why = "bad stored size"; return false; }
        payload.assign(reinterpret_cast<const char*>(p + kHeader), raw);
    } else if (method == kXpressHuff) {
        if (!XpressUnpack(p + kHeader, packed, raw, payload)) { why = "decompression failed"; return false; }
    } else { why = "unknown method " + std::to_string(method); return false; }
    if (Crc32(payload.data(), payload.size()) != crc) { why = "CRC mismatch"; return false; }
    size_t at = payload.find('\n');
    if (at == std::string::npos || payload.compare(0, at, kFirstLine) != 0) { why = "bad first line"; return false; }
    for (++at; at < payload.size();) {
        const size_t end = payload.find('\n', at);
        if (end == std::string::npos) { why = "unterminated line"; set.clear(); return false; }
        const std::string line = payload.substr(at, end - at);
        at = end + 1;
        if (line.empty()) continue;
        const size_t tab = line.find('\t');
        std::wstring text;
        if (tab == std::string::npos || !ProfileName(line.substr(0, tab)) || !Wide(line.substr(tab + 1), text)) { why = "bad line"; set.clear(); return false; }
        set[line.substr(0, tab)] = text;
    }
    return true;
}
}
