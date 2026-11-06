#include "env_checks.h"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace EnvCheck {
namespace {
// Section-mapped view of a PE32+ file read from disk.
struct Image {
    const std::vector<uint8_t>& file;
    const IMAGE_NT_HEADERS64* nt = nullptr;
    const IMAGE_SECTION_HEADER* sections = nullptr;
    unsigned count = 0;

    explicit Image(const std::vector<uint8_t>& f) : file(f) {}
    bool Open(std::string& why) {
        if (file.size() < sizeof(IMAGE_DOS_HEADER)) { why = "file too small"; return false; }
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(file.data());
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
            static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > file.size()) { why = "not a PE file"; return false; }
        nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(file.data() + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) { why = "not a PE file"; return false; }
        if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
            why = "not an x64 image"; return false;
        }
        const size_t at = static_cast<size_t>(dos->e_lfanew) + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt->FileHeader.SizeOfOptionalHeader;
        count = nt->FileHeader.NumberOfSections;
        if (at + static_cast<size_t>(count) * sizeof(IMAGE_SECTION_HEADER) > file.size()) { why = "section table outside the file"; return false; }
        sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(file.data() + at);
        return true;
    }
    // File offset of an RVA with at least `bytes` behind it in the same section; SIZE_MAX when outside.
    size_t Offset(uint32_t rva, size_t bytes = 1) const {
        for (unsigned i = 0; i < count; ++i) {
            const IMAGE_SECTION_HEADER& s = sections[i];
            const uint32_t size = s.SizeOfRawData > s.Misc.VirtualSize && s.Misc.VirtualSize ? s.Misc.VirtualSize : s.SizeOfRawData;
            if (rva >= s.VirtualAddress && rva - s.VirtualAddress < size) {
                const size_t off = static_cast<size_t>(s.PointerToRawData) + (rva - s.VirtualAddress);
                if (bytes > static_cast<size_t>(size - (rva - s.VirtualAddress)) || off + bytes > file.size()) return SIZE_MAX;
                return off;
            }
        }
        return SIZE_MAX;
    }
    template<class T> const T* At(uint32_t rva) const {
        const size_t off = Offset(rva, sizeof(T));
        return off == SIZE_MAX ? nullptr : reinterpret_cast<const T*>(file.data() + off);
    }
    // NUL-terminated ASCII at an RVA, at most 512 characters.
    bool String(uint32_t rva, std::string& out) const {
        const size_t off = Offset(rva);
        if (off == SIZE_MAX) return false;
        out.clear();
        for (size_t i = off; i < file.size() && out.size() < 512; ++i) {
            if (!file[i]) return true;
            out.push_back(static_cast<char>(file[i]));
        }
        return false;
    }
    IMAGE_DATA_DIRECTORY Directory(unsigned index) const {
        if (index >= nt->OptionalHeader.NumberOfRvaAndSizes) return {};
        return nt->OptionalHeader.DataDirectory[index];
    }
};

// One import name table (64-bit thunks) into a module entry.
bool ReadThunks(const Image& image, uint32_t rva, ImportModule& module, std::string& why) {
    for (unsigned n = 0; n < 65536; ++n, rva += 8u) {
        const uint64_t* thunk = image.At<uint64_t>(rva);
        if (!thunk) { why = "import thunks outside the file (" + module.name + ")"; return false; }
        if (!*thunk) return true;
        if (*thunk & IMAGE_ORDINAL_FLAG64) { module.ordinals.push_back(static_cast<uint16_t>(*thunk & 0xffff)); continue; }
        std::string name;
        if (!image.String(static_cast<uint32_t>(*thunk) + 2u, name)) { why = "import name outside the file (" + module.name + ")"; return false; }
        module.functions.push_back(name);
    }
    why = "import table not terminated (" + module.name + ")";
    return false;
}
}

bool IsX64Image(const std::vector<uint8_t>& file) {
    Image image(file);
    std::string why;
    return image.Open(why);
}

bool ReadImports(const std::vector<uint8_t>& file, std::vector<ImportModule>& out, std::string& why) {
    out.clear();
    Image image(file);
    if (!image.Open(why)) return false;
    const IMAGE_DATA_DIRECTORY imports = image.Directory(IMAGE_DIRECTORY_ENTRY_IMPORT);
    for (uint32_t rva = imports.VirtualAddress; imports.VirtualAddress; rva += static_cast<uint32_t>(sizeof(IMAGE_IMPORT_DESCRIPTOR))) {
        const auto* d = image.At<IMAGE_IMPORT_DESCRIPTOR>(rva);
        if (!d) { why = "import directory outside the file"; return false; }
        if (!d->Name && !d->FirstThunk) break;
        ImportModule module;
        if (!image.String(d->Name, module.name)) { why = "import module name outside the file"; return false; }
        if (!ReadThunks(image, d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk, module, why)) return false;
        out.push_back(std::move(module));
        if (out.size() > 512) { why = "too many import modules"; return false; }
    }
    const IMAGE_DATA_DIRECTORY delay = image.Directory(IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT);
    for (uint32_t rva = delay.VirtualAddress; delay.VirtualAddress; rva += static_cast<uint32_t>(sizeof(IMAGE_DELAYLOAD_DESCRIPTOR))) {
        const auto* d = image.At<IMAGE_DELAYLOAD_DESCRIPTOR>(rva);
        if (!d) { why = "delay-import directory outside the file"; return false; }
        if (!d->DllNameRVA) break;
        ImportModule module;
        module.delay = true;
        // Pre-VC7 tables hold virtual addresses instead of RVAs; nothing this tool reads is that old.
        if (!d->Attributes.RvaBased) { why = "delay-import table is not RVA based"; return false; }
        if (!image.String(d->DllNameRVA, module.name)) { why = "delay-import module name outside the file"; return false; }
        if (!ReadThunks(image, d->ImportNameTableRVA, module, why)) return false;
        out.push_back(std::move(module));
        if (out.size() > 512) { why = "too many import modules"; return false; }
    }
    return true;
}

bool ReadExports(const std::vector<uint8_t>& file, std::vector<std::string>& out, std::string& why) {
    out.clear();
    Image image(file);
    if (!image.Open(why)) return false;
    const IMAGE_DATA_DIRECTORY exports = image.Directory(IMAGE_DIRECTORY_ENTRY_EXPORT);
    if (!exports.VirtualAddress) return true;
    const auto* d = image.At<IMAGE_EXPORT_DIRECTORY>(exports.VirtualAddress);
    if (!d) { why = "export directory outside the file"; return false; }
    if (d->NumberOfNames > 65536) { why = "too many exports"; return false; }
    for (DWORD i = 0; i < d->NumberOfNames; ++i) {
        const DWORD* name = image.At<DWORD>(d->AddressOfNames + i * 4u);
        std::string text;
        if (!name || !image.String(*name, text)) { why = "export name outside the file"; return false; }
        out.push_back(text);
    }
    return true;
}

DllKind ClassifyDll(const std::vector<uint8_t>& file) {
    auto has = [&](const char* mark) {
        const size_t n = std::strlen(mark);
        std::string wide;
        for (size_t i = 0; i < n; ++i) { wide.push_back(mark[i]); wide.push_back('\0'); }
        const auto contains = [&](const char* needle, size_t size) {
            if (file.size() < size) return false;
            for (size_t i = 0; i + size <= file.size(); ++i)
                if (file[i] == static_cast<uint8_t>(needle[0]) && std::memcmp(file.data() + i, needle, size) == 0) return true;
            return false;
        };
        return contains(mark, n) || contains(wide.data(), wide.size());
    };
    if (has("PBZDebugMenu")) return DllKind::Full;
    if (has("PBZTriggersOff")) return DllKind::TriggersOnly;
    return DllKind::Foreign;
}

LastRun ParseLog(const std::string& text) {
    LastRun run;
    // The last line that carries a pid names the run.
    size_t end = text.size();
    while (end > 0) {
        size_t begin = text.rfind('\n', end - 1);
        begin = begin == std::string::npos ? 0 : begin + 1;
        const std::string line = text.substr(begin, end - begin);
        const size_t at = line.find(" pid=");
        if (at != std::string::npos) { run.pid = static_cast<unsigned>(std::strtoul(line.c_str() + at + 5, nullptr, 10)); break; }
        if (!begin) break;
        end = begin - 1;
    }
    if (!run.pid) return run;
    const std::string tag = " pid=" + std::to_string(run.pid) + " ";
    for (size_t begin = 0; begin < text.size();) {
        size_t stop = text.find('\n', begin);
        if (stop == std::string::npos) stop = text.size();
        const std::string line = text.substr(begin, stop - begin);
        begin = stop + 1;
        const size_t at = line.find(tag);
        if (at == std::string::npos) continue;
        const std::string stamp = line.substr(0, at);
        if (run.first.empty()) run.first = stamp;
        run.last = stamp;
        ++run.lines;
        const std::string body = line.substr(at + tag.size());
        if (!body.compare(0, 11, "KNOWN_BUILD")) run.start = body.find("WeGame") != std::string::npos ? LastRun::Start::WeGame : LastRun::Start::Steam;
        else if (!body.compare(0, 17, "UNSUPPORTED_BUILD")) run.start = LastRun::Start::UnsupportedBuild;
        else if (!body.compare(0, 16, "UNSUPPORTED_HOST")) run.start = LastRun::Start::UnsupportedHost;
        else if (!body.compare(0, 10, "PIN_FAILED")) run.start = LastRun::Start::PinFailed;
        else if (!body.compare(0, 22, "UI_LOAD_BANNER success")) run.banner = true;
        else if (!body.compare(0, 34, "UE_RUNTIME reference helpers misma")) run.refMismatch = true;
        else if (!body.compare(0, 16, "GAME_THREAD_HOOK") && body.find("not installed") != std::string::npos) run.hookWaiting = true;
    }
    run.found = run.lines > 0;
    return run;
}

std::vector<RunSpan> ParseRuns(const std::string& text) {
    std::vector<RunSpan> runs;
    for (size_t begin = 0; begin < text.size();) {
        size_t stop = text.find('\n', begin);
        if (stop == std::string::npos) stop = text.size();
        const std::string line = text.substr(begin, stop - begin);
        begin = stop + 1;
        // "yyyy-mm-dd hh:mm:ss pid=N ..."
        if (line.size() < 25 || line.compare(19, 5, " pid=")) continue;
        char* end = nullptr;
        const unsigned pid = static_cast<unsigned>(std::strtoul(line.c_str() + 24, &end, 10));
        if (!pid || !end || *end != ' ') continue;
        const std::string stamp = line.substr(0, 19);
        RunSpan* run = nullptr;
        for (size_t i = runs.size(); i-- > 0;) if (runs[i].pid == pid) { run = &runs[i]; break; }
        if (!run) { runs.push_back({pid, stamp, stamp, true}); run = &runs.back(); }
        run->last = stamp;
        if (!line.compare(static_cast<size_t>(end - line.c_str()) + 1, 16, "UNSUPPORTED_HOST")) run->host = false;
    }
    return runs;
}

uint32_t LoaderBlockers(const std::vector<uint8_t>& options) {
    auto qword = [&](size_t i) {
        uint64_t v = 0;
        if (options.size() >= (i + 1) * 8) std::memcpy(&v, options.data() + i * 8, 8);
        return v;
    };
    // Every policy is a 4-bit field: 0 use default, 1 always on, 2 always off, 3 a stricter "on" variant for some.
    auto field = [](uint64_t v, unsigned shift) { return static_cast<unsigned>((v >> shift) & 0xF); };
    const uint64_t first = qword(0), second = qword(1);
    uint32_t out = 0;
    if (field(first, 60) == 1) out |= kPreferSystem32;                                       // IMAGE_LOAD_PREFER_SYSTEM32
    if (field(first, 44) == 1 || field(first, 44) == 3) out |= kMicrosoftSignedOnly;         // BLOCK_NON_MICROSOFT_BINARIES (3 = also Store)
    if (field(first, 52) == 1) out |= kNoRemoteImages;                                       // IMAGE_LOAD_NO_REMOTE
    if (field(first, 56) == 1) out |= kNoLowLabelImages;                                     // IMAGE_LOAD_NO_LOW_LABEL
    if (field(second, 36) == 1 || field(second, 36) == 3) out |= kNonCetBlocked;             // BLOCK_NON_CET_BINARIES (3 = non-EHCONT)
    return out;
}

bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& out, size_t limit) {
    out.clear();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart >= 0 && static_cast<unsigned long long>(size.QuadPart) <= limit;
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        for (size_t at = 0; ok && at < out.size(); at += read) {
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(out.size() - at, 1u << 24));
            ok = ReadFile(file, out.data() + at, chunk, &read, nullptr) && read;
        }
    }
    CloseHandle(file);
    if (!ok) out.clear();
    return ok;
}

std::string Sha256File(const std::wstring& path, std::string& why) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) { why = "cannot open (error " + std::to_string(GetLastError()) + ")"; return {}; }
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string hex;
    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
        BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0))) {
        std::vector<uint8_t> buffer(4u << 20);
        DWORD read = 0;
        bool ok = true;
        while (ok && ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read)
            ok = BCRYPT_SUCCESS(BCryptHashData(hash, buffer.data(), read, 0));
        uint8_t digest[32]{};
        if (ok && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0))) {
            static const char kHex[] = "0123456789abcdef";
            for (uint8_t b : digest) { hex.push_back(kHex[b >> 4]); hex.push_back(kHex[b & 15]); }
        } else why = "read or hash failed";
    } else why = "SHA-256 provider unavailable";
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    return hex;
}
}
