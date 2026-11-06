#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// .combo: one set of move chains (招式派生: the hero's and each NPC look's) in a file of its own, under combos\ next
// to version.dll. Layout, little endian:
//   "PBZCOMBO" (8 bytes) | uint16 version (1) | uint16 method (0 stored, 1 XPRESS_HUFF via cabinet.dll)
//   | uint32 payload size | uint32 CRC-32 of the payload | uint32 packed size | packed payload
// Payload, UTF-8 text: a first line "PBZCOMBO 1", then one line per look: "<profile>\t<chain text>\n" (profile: letters,
// digits, '_'; chain text: NpcCombo::Encode). A file that fails any check is not read at all.
namespace ComboFile {
constexpr uint16_t kVersion = 1;
constexpr size_t kHeader = 24, kMaxPayload = 8u << 20;
using Set = std::map<std::string, std::wstring>;  // profile -> chain text
uint32_t Crc32(const void* data, size_t size);
bool ProfileName(const std::string& profile);
// compress: XPRESS_HUFF when that is smaller (else stored).
std::vector<uint8_t> Pack(const Set& set, bool compress = true);
// False, with why, on a bad magic / version / method / size / CRC / decompression / line.
bool Unpack(const std::vector<uint8_t>& bytes, Set& set, std::string& why);
}
