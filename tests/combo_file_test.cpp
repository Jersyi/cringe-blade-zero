// .combo files (ComboFile) and the chain store on top of them (NpcCombo): format round trip, bad files skipped,
// the first-run migration from PBZDebugMenu.ini, save-as / select, and a change on disk read again.
#include "combo_file.h"
#include "npc_combo.h"
#include "settings_store.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
int failures = 0;
void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failures; }
// The ability of the chain's first move (whatever else the decoder adds, such as the key node).
std::string FirstMove(const NpcCombo::Config& c) { for (const NpcCombo::Node& n : c.chain) if (n.type == NpcCombo::NodeType::Skill) return n.ability; return {}; }
std::vector<uint8_t> ReadAll(const std::wstring& path) {
    std::vector<uint8_t> bytes;
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return bytes;
    LARGE_INTEGER size{}; GetFileSizeEx(file, &size); bytes.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0; ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr); CloseHandle(file);
    return bytes;
}
void WriteAll(const std::wstring& path, const std::vector<uint8_t>& bytes) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD wrote = 0; WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr); CloseHandle(file);
}
}

int wmain() {
    using ComboFile::Set;
    // ---- the format ----
    Set set;
    set["Hero"] = L"E1|T:1,1200,1,10,20,1|S:GA_Hero_Light_01,200,40,-1";
    set["MoTian"] = L"E1|X:MuXiaoKui,GA_MuXiaoKui_Skill_01,300,40,-1,450";
    std::wstring big = L"E1"; for (int i = 0; i < 40; ++i) big += L"|S:GA_LongName_Skill_" + std::to_wstring(i) + L"," + std::to_wstring(i * 10) + L",0,-1";
    set["ZuoShang"] = big;
    set["bad name!"] = L"E1";  // not written
    const std::vector<uint8_t> packed = ComboFile::Pack(set), stored = ComboFile::Pack(set, false);
    Set back; std::string why;
    Check(packed.size() > ComboFile::kHeader && std::memcmp(packed.data(), "PBZCOMBO", 8) == 0 && packed[10] == 1, "a set packs with the magic and XPRESS_HUFF");
    Check(packed.size() < stored.size(), "compressed is smaller than stored");
    Check(ComboFile::Unpack(packed, back, why) && back.size() == 3 && back["ZuoShang"] == big && back["Hero"] == set["Hero"] && !back.count("bad name!"),
          "it reads back the same chains (a bad profile name is never written)");
    Check(ComboFile::Unpack(stored, back, why) && back.size() == 3, "a stored (uncompressed) file reads too");
    std::vector<uint8_t> broken = packed; broken[broken.size() / 2] ^= 0x5A;
    Check(!ComboFile::Unpack(broken, back, why) && back.empty(), "a flipped byte fails (CRC / decompression)");
    std::printf("  why=%s\n", why.c_str());
    broken = stored; broken.back() ^= 0x01;
    Check(!ComboFile::Unpack(broken, back, why) && why == "CRC mismatch", "a stored file with a changed byte fails its CRC");
    broken = packed; broken[0] = 'X';
    Check(!ComboFile::Unpack(broken, back, why) && why.find("magic") != std::string::npos, "a wrong magic is not a .combo file");
    broken = packed; broken[8] = 9;
    Check(!ComboFile::Unpack(broken, back, why) && why.find("version") != std::string::npos, "an unknown version is refused");
    broken.assign(packed.begin(), packed.begin() + 30);
    Check(!ComboFile::Unpack(broken, back, why), "a cut-off file is refused");
    Set chinese; chinese["Hero"] = L"E1|S:GA_中文,0,0,-1";
    Check(ComboFile::Unpack(ComboFile::Pack(chinese), back, why) && back["Hero"] == chinese["Hero"], "UTF-8 text survives");

    // ---- the store: migration, save, save-as, select, bad file, live reload ----
    wchar_t temp[MAX_PATH]{}; GetTempPathW(MAX_PATH, temp);
    const std::wstring root = std::wstring(temp) + L"pbz_combo_test_" + std::to_wstring(GetCurrentProcessId());
    const std::wstring folder = root + L"\\combos", ini = root + L"\\PBZDebugMenu.ini";
    CreateDirectoryW(root.c_str(), nullptr);
    WritePrivateProfileStringW(L"NpcCombo", L"MoTian", L"E1|T:1,1200,1,10,20,1|S:GA_MoTian_Skill_01,200,40,-1", ini.c_str());
    WritePrivateProfileStringW(L"NpcCombo", L"Hero", L"E1|T:2,1200,1,10,20,1|S:GA_Hero_Heavy,200,40,-1", ini.c_str());
    SettingsStore::Initialize(ini.c_str());
    NpcCombo::SetFolder(folder);
    const uint64_t g0 = NpcCombo::Generation();
    const NpcCombo::Config motian = NpcCombo::Load("MoTian");
    NpcCombo::Files files = NpcCombo::GetFiles();
    Check(files.names.size() == 1 && files.names[0] == L"default" && files.active == L"default" && GetFileAttributesW((folder + L"\\default.combo").c_str()) != INVALID_FILE_ATTRIBUTES,
          "first run: the ini's chains move into combos\\default.combo");
    Check(FirstMove(motian) == "GA_MoTian_Skill_01" && FirstMove(NpcCombo::Load("Hero")) == "GA_Hero_Heavy",
          "the migrated chains load (an NPC look's and the hero's)");
    Check(NpcCombo::Load("YuZan").chain.size() == 2 && NpcCombo::KeyTrigger(NpcCombo::Load("YuZan")) >= 0 && NpcCombo::PreInputMs(NpcCombo::Load("YuZan")) == NpcCombo::kPreInputDefault &&
          NpcCombo::PreInputNode(NpcCombo::Load("YuZan")) >= 0, "a look without a chain starts with the key node and its 0.3 s pre-input node");
    NpcCombo::Config edited = motian; for (NpcCombo::Node& n : edited.chain) if (n.type == NpcCombo::NodeType::Skill) n.ability = "GA_MoTian_Skill_02";
    NpcCombo::Save("MoTian", edited);
    Set on_disk;
    Check(ComboFile::Unpack(ReadAll(folder + L"\\default.combo"), on_disk, why) && on_disk["MoTian"].find(L"GA_MoTian_Skill_02") != std::wstring::npos && NpcCombo::Generation() > g0,
          "Save writes the active file at once and bumps the generation");
    Check(NpcCombo::SaveAs(L"进攻") && NpcCombo::GetFiles().active == L"进攻" && NpcCombo::GetFiles().names.size() == 2, "save-as makes a second file the active one");
    NpcCombo::Config other = edited; for (NpcCombo::Node& n : other.chain) if (n.type == NpcCombo::NodeType::Skill) n.ability = "GA_MoTian_Skill_03"; NpcCombo::Save("MoTian", other);
    Check(NpcCombo::SelectFile(L"default") && FirstMove(NpcCombo::Load("MoTian")) == "GA_MoTian_Skill_02" && SettingsStore::Get(L"NpcCombo", L"File") == L"default",
          "selecting a file loads its set (and is remembered)");
    Check(NpcCombo::SelectFile(L"进攻") && FirstMove(NpcCombo::Load("MoTian")) == "GA_MoTian_Skill_03", "and back: each file keeps its own chains");
    WriteAll(folder + L"\\broken.combo", broken);
    NpcCombo::Poll();
    Check(!NpcCombo::SelectFile(L"broken") && NpcCombo::GetFiles().active == L"进攻" && !NpcCombo::GetFiles().status.empty() &&
          FirstMove(NpcCombo::Load("MoTian")) == "GA_MoTian_Skill_03", "a bad file is skipped: the loaded set stays, the status says why");
    Check(!NpcCombo::SaveAs(L"a/b") && !NpcCombo::SaveAs(L""), "bad file names are refused");
    // Another program writes the active file: read again by Poll.
    Sleep(50);
    Set outside; outside["MoTian"] = L"E1|T:1,1200,1,10,20,1|S:GA_MoTian_Skill_09,200,40,-1";
    WriteAll(folder + L"\\进攻.combo", ComboFile::Pack(outside));
    const uint64_t g1 = NpcCombo::Generation();
    Check(NpcCombo::Poll() && FirstMove(NpcCombo::Load("MoTian")) == "GA_MoTian_Skill_09" && NpcCombo::Generation() > g1, "a change on disk is read again (live reload)");
    Check(!NpcCombo::Poll(), "nothing changed: nothing reloaded");
    Sleep(50);
    WriteAll(folder + L"\\进攻.combo", broken);
    Check(!NpcCombo::Poll() && FirstMove(NpcCombo::Load("MoTian")) == "GA_MoTian_Skill_09" && !NpcCombo::GetFiles().status.empty(),
          "a bad change on disk is skipped and the loaded set stays");
    // A second run with files present: no migration, the remembered file is active.
    NpcCombo::SetFolder(folder);
    Check(NpcCombo::GetFiles().names.size() == 3 && NpcCombo::GetFiles().active == L"进攻" && !NpcCombo::GetFiles().status.empty(),
          "files present: no migration; the remembered file is active again (bad now: reported, nothing loaded)");

    for (const wchar_t* name : {L"default", L"进攻", L"broken"}) DeleteFileW((folder + L"\\" + name + L".combo").c_str());
    RemoveDirectoryW(folder.c_str()); DeleteFileW(ini.c_str()); RemoveDirectoryW(root.c_str());
    std::printf("combo_file_test failures=%d\n", failures);
    return failures ? 1 : 0;
}
