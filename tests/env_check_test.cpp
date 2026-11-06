// EnvCheck.exe pure parts: PE import / export reading, DLL classification, last-run log parsing, file SHA-256, and the
// PBZ.exe builds generated from proxy.cpp.
#include "env_checks.h"
#include "known_exes.h"
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}
std::wstring System32() {
    wchar_t path[MAX_PATH]{};
    GetSystemDirectoryW(path, MAX_PATH);
    return path;
}
std::wstring SelfPath() {
    wchar_t path[MAX_PATH * 4]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH * 4);
    return path;
}
bool HasModule(const std::vector<EnvCheck::ImportModule>& modules, const char* name, const char* function) {
    for (const auto& m : modules)
        if (!_stricmp(m.name.c_str(), name))
            for (const auto& f : m.functions) if (f == function) return true;
    return false;
}
}

int main() {
    // This test's own imports: kernel32 and bcrypt (EnvCheck::Sha256File) by name.
    std::vector<uint8_t> self;
    Check(EnvCheck::ReadFileBytes(SelfPath(), self), "read own exe");
    Check(EnvCheck::IsX64Image(self), "own exe is x64");
    std::vector<EnvCheck::ImportModule> imports;
    std::string why;
    Check(EnvCheck::ReadImports(self, imports, why), "own imports read");
    Check(HasModule(imports, "KERNEL32.dll", "GetSystemDirectoryW"), "kernel32!GetSystemDirectoryW imported");
    Check(HasModule(imports, "bcrypt.dll", "BCryptOpenAlgorithmProvider"), "bcrypt!BCryptOpenAlgorithmProvider imported");

    // System32\version.dll exports the 17 functions the proxy forwards.
    std::vector<uint8_t> version;
    std::vector<std::string> exports;
    Check(EnvCheck::ReadFileBytes(System32() + L"\\version.dll", version) && EnvCheck::ReadExports(version, exports, why), "system version.dll exports read");
    bool fileInfo = false;
    for (const auto& e : exports) fileInfo = fileInfo || e == "GetFileVersionInfoW";
    Check(exports.size() >= 17 && fileInfo, "system version.dll has GetFileVersionInfoW and >= 17 exports");
    Check(EnvCheck::ClassifyDll(version) == EnvCheck::DllKind::Foreign, "system version.dll is foreign");

    // Garbage and truncated images are refused, not read past their end.
    std::vector<uint8_t> junk(4096, 0x5a);
    Check(!EnvCheck::IsX64Image(junk) && !EnvCheck::ReadImports(junk, imports, why), "junk refused");
    std::vector<uint8_t> cut(self.begin(), self.begin() + 1024);
    Check(!EnvCheck::ReadImports(cut, imports, why), "truncated image refused");
    std::vector<uint8_t> wrongMachine = self;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(wrongMachine.data());
    reinterpret_cast<IMAGE_NT_HEADERS64*>(wrongMachine.data() + dos->e_lfanew)->FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
    Check(!EnvCheck::IsX64Image(wrongMachine), "x86 machine refused");

    // Markers: narrow / UTF-16, full before triggers-only.
    auto bytesOf = [](const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); };
    Check(EnvCheck::ClassifyDll(bytesOf(std::string("xxPBZDebugMenu.logyy"))) == EnvCheck::DllKind::Full, "narrow PBZDebugMenu = full");
    Check(EnvCheck::ClassifyDll(bytesOf(std::string("P\0B\0Z\0T\0r\0i\0g\0g\0e\0r\0s\0O\0f\0f\0", 28))) == EnvCheck::DllKind::TriggersOnly, "UTF-16 PBZTriggersOff = triggers-only");
    Check(EnvCheck::ClassifyDll(bytesOf(std::string("PBZTriggersOff PBZDebugMenu"))) == EnvCheck::DllKind::Full, "both markers = full");
    Check(EnvCheck::ClassifyDll(bytesOf(std::string("PBZDebugMen"))) == EnvCheck::DllKind::Foreign, "partial marker = foreign");

    // Log: the last pid's run only.
    const std::string log =
        "2026-11-06 12:00:00 pid=100 KNOWN_BUILD sha256 verified\n"
        "2026-11-06 12:00:05 pid=100 UI_LOAD_BANNER success; main tool window remains hidden until user request\n"
        "2026-11-06 13:50:54 pid=200 PAD adaptive triggers FORCED OFF\n"
        "2026-11-06 13:50:54 pid=200 KNOWN_BUILD sha256 verified build=WeGame\n"
        "2026-11-06 13:51:00 pid=200 UE_RUNTIME reference helpers mismatch: engine AddRef/ReleaseRef differ\n"
        "2026-11-06 13:53:43 pid=200 NPC_BLINK shown actors=2/2 after=change\n";
    EnvCheck::LastRun run = EnvCheck::ParseLog(log);
    Check(run.found && run.pid == 200 && run.lines == 4, "last run = pid 200, 4 lines");
    Check(run.start == EnvCheck::LastRun::Start::WeGame && !run.banner && run.refMismatch, "WeGame start, no banner, reference mismatch");
    Check(run.first == "2026-11-06 13:50:54" && run.last == "2026-11-06 13:53:43", "first / last timestamps");
    run = EnvCheck::ParseLog("2026-11-06 12:00:00 pid=7 UNSUPPORTED_BUILD; no data writes\n");
    Check(run.start == EnvCheck::LastRun::Start::UnsupportedBuild, "unsupported build");
    run = EnvCheck::ParseLog("no pid here\n");
    Check(!run.found, "no pid = nothing found");
    run = EnvCheck::ParseLog("2026-11-06 12:00:00 pid=8 KNOWN_BUILD sha256 verified\n2026-11-06 12:00:01 pid=8 UI_LOAD_BANNER success; x\n\n");
    Check(run.found && run.start == EnvCheck::LastRun::Start::Steam && run.banner, "Steam start with banner, trailing blank line");

    // Run spans: every pid, first / last stamp, other hosts marked.
    const std::vector<EnvCheck::RunSpan> runs = EnvCheck::ParseRuns(log + "2026-11-06 14:00:00 pid=300 UNSUPPORTED_HOST; no data writes\nnot a log line\n");
    Check(runs.size() == 3 && runs[0].pid == 100 && runs[0].first == "2026-11-06 12:00:00" && runs[0].last == "2026-11-06 12:00:05" && runs[0].host,
          "spans: pid 100 12:00:00-12:00:05");
    Check(runs[1].pid == 200 && runs[1].first == "2026-11-06 13:50:54" && runs[1].last == "2026-11-06 13:53:43", "spans: pid 200 13:50:54-13:53:43");
    Check(runs[2].pid == 300 && !runs[2].host, "spans: UNSUPPORTED_HOST run is not the game");
    Check(EnvCheck::ParseRuns("2026-11-06 12:00:00 pid=x KNOWN\n12:00:00 pid=5 y\n").empty(), "spans: malformed lines skipped");

    // Exploit-protection fields that block the mod.
    auto options = [](uint64_t first, uint64_t second, size_t bytes) {
        std::vector<uint8_t> v(bytes, 0);
        std::memcpy(v.data(), &first, std::min<size_t>(8, bytes));
        if (bytes >= 16) std::memcpy(v.data() + 8, &second, 8);
        return v;
    };
    Check(EnvCheck::LoaderBlockers(options(1ull << 60, 0, 8)) == EnvCheck::kPreferSystem32, "prefer System32 images");
    Check(EnvCheck::LoaderBlockers(options(2ull << 60, 0, 8)) == 0, "prefer System32 forced off = fine");
    Check(EnvCheck::LoaderBlockers(options(3ull << 44, 0, 16)) == EnvCheck::kMicrosoftSignedOnly, "Microsoft-signed only (Store variant)");
    Check(EnvCheck::LoaderBlockers(options((1ull << 52) | (1ull << 56), 1ull << 36, 24)) ==
              (EnvCheck::kNoRemoteImages | EnvCheck::kNoLowLabelImages | EnvCheck::kNonCetBlocked), "remote + low label + non-CET");
    Check(EnvCheck::LoaderBlockers(options(0x0000000000000111ull | (1ull << 40), 1ull << 28, 16)) == 0, "DEP / CFG / shadow stacks do not block");
    Check(EnvCheck::LoaderBlockers({}) == 0 && EnvCheck::LoaderBlockers(options(1ull << 60, 0, 4)) == 0, "short values ignored");

    // SHA-256 of "abc".
    wchar_t temp[MAX_PATH]{}, file[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    GetTempFileNameW(temp, L"env", 0, file);
    HANDLE h = CreateFileW(file, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD written = 0;
    WriteFile(h, "abc", 3, &written, nullptr);
    CloseHandle(h);
    Check(EnvCheck::Sha256File(file, why) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256(abc)");
    DeleteFileW(file);

    // Builds generated from proxy.cpp.
    Check(std::size(kKnownExes) == 2 && kKnownExes[0].bytes > 100000000ull && std::strlen(kKnownExes[0].sha256) == 64 &&
              kKnownExes[1].bytes > 100000000ull && std::strlen(kKnownExes[1].sha256) == 64, "known_exes.h: Steam + WeGame size and SHA-256");

    std::printf("env_check_test failures=%d\n", failures);
    return failures ? 1 : 0;
}
