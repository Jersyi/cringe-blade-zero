// Actual Encode/Decode + compressed store + atomic disk replacement, isolated from player files.
#include "npc_combo.h"
#include "combo_file.h"
#include "settings_store.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

namespace {
int failures=0,checks=0,saves=0,reloads=0;
void Check(bool ok,const char* what) { ++checks; if (!ok) { ++failures; std::printf("FAIL %s\n",what); } }
std::vector<uint8_t> Read(const std::filesystem::path& path) {
    const HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    if (file==INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size{}; GetFileSizeEx(file,&size);
    std::vector<uint8_t> bytes(static_cast<size_t>(size.QuadPart)); DWORD used=0;
    const bool ok=ReadFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&used,nullptr)&&used==bytes.size(); CloseHandle(file);
    if (!ok) bytes.clear(); return bytes;
}
bool Write(const std::filesystem::path& path,const std::vector<uint8_t>& bytes) {
    const HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,nullptr);
    if (file==INVALID_HANDLE_VALUE) return false;
    DWORD used=0; const bool ok=WriteFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&used,nullptr)&&used==bytes.size(); CloseHandle(file); return ok;
}
NpcCombo::Config Fixture(size_t count,int salt) {
    using namespace NpcCombo;
    Config c=Decode(L"E1"); EnsurePreInput(c); EnsureAirDefault(c); c.placed=true;
    std::vector<NodeType> types;
    for (int t=0;t<=static_cast<int>(NodeType::AirDefault);++t)
        if (t!=static_cast<int>(NodeType::PreInput)&&t!=static_cast<int>(NodeType::AirDefault)) types.push_back(static_cast<NodeType>(t));
    while (c.chain.size()<count) {
        const size_t i=c.chain.size(); Node n;
        n.type=types[i%types.size()]; n.x=static_cast<int>(i%50)*280+salt; n.y=static_cast<int>(i/50)*200;
        n.ability="GA_Stress_Skill_"+std::to_string(i); n.boss="MoTian"; n.weapon="TangBlade"; n.ms=350;
        n.trigger=static_cast<int>(Trigger::Light); n.speed=125; n.own_look=true;
        if (n.type==NodeType::Branch||n.type==NodeType::WeaponActions) {
            if (n.type==NodeType::WeaponActions&&i%2) n=AirSetNode(n.x,n.y);
            n.routes.clear();
            for (size_t j=0;j<kMaxRoutes;++j) { Route route; route.button=static_cast<int>(Trigger::Light)+static_cast<int>(j); route.pause_ms=static_cast<int>(j)*100; route.next=static_cast<int>((i+j+1)%count); n.routes.push_back(route); }
        }
        if (n.type==NodeType::Speed) { n.curve=true; n.points=DefaultCurve(125); }
        if (n.type==NodeType::Hook) { n.hook_auto=true; n.hook_dist=1800; n.hook_speed=150; }
        if (n.type==NodeType::Group) { n.w=500; n.h=360; }
        n.next=static_cast<int>((i+1)%count);
        c.chain.push_back(n);
    }
    // Normalize the generated fixture through the same authored constraints as the editor.
    return Decode(Encode(c));
}
}
int wmain() {
    namespace fs=std::filesystem;
    const auto start=std::chrono::steady_clock::now();
    const fs::path root=fs::temp_directory_path()/(L"pbz_combo_save_stress_"+std::to_wstring(GetCurrentProcessId())+L"_"+std::to_wstring(GetTickCount64()));
    if (!fs::create_directory(root)) return 2;
    const fs::path folder=root/L"isolated"; fs::create_directory(folder);
    SettingsStore::Initialize((root/L"test.ini").c_str());
    SettingsStore::Put(L"NpcCombo",L"File",L"stress");
    if (!Write(folder/L"stress.combo",ComboFile::Pack({}))) return 2; // seed before Ready: never create default
    NpcCombo::SetFolder(folder.wstring());
    Check(NpcCombo::GetFiles().active==L"stress","isolated file selected before any Save");
    for (size_t count:{10u,100u,1000u,10000u}) {
        auto c=Fixture(count,0); Check(c.chain.size()==count,"fixture retains requested node count");
        for (int round=0;round<25;++round) {
            c.chain.back().x+=1;
            const std::wstring expected=NpcCombo::Encode(c);
            NpcCombo::Save("Hero",c); ++saves;
            ComboFile::Set disk; std::string why;
            Check(ComboFile::Unpack(Read(folder/L"stress.combo"),disk,why)&&disk["Hero"]==expected,"every saved file unpacks exactly");
            Check(NpcCombo::SelectFile(L"stress"),"reload written file"); ++reloads;
            const auto actual=NpcCombo::Encode(NpcCombo::Load("Hero"));
            Check(actual==expected,"all node data survives forced disk reload");
        }
    }
    const auto large=Fixture(10000,19); const auto large_text=NpcCombo::Encode(large);
    for (int profile=0;profile<8;++profile) { NpcCombo::Save("Stress_"+std::to_string(profile),large); ++saves; }
    Check(NpcCombo::SaveAs(L"stress_copy"),"large multi-profile save-as"); ++saves;
    for (int round=0;round<12;++round) {
        Check(NpcCombo::SelectFile(round%2?L"stress":L"stress_copy"),"switch large files"); ++reloads;
        for (int profile=0;profile<8;++profile)
            Check(NpcCombo::Encode(NpcCombo::Load("Stress_"+std::to_string(profile)))==large_text,"10000-node profile survives save-as and file switching");
    }
    // Use a fresh file for concurrent store access so the flush cost stays bounded.
    Check(Write(folder/L"concurrent.combo",ComboFile::Pack({}))&&NpcCombo::SelectFile(L"concurrent"),"isolated concurrent file ready");
    auto concurrent=Fixture(1000,7); NpcCombo::Save("Concurrent",concurrent); ++saves;
    std::atomic<bool> done=false; std::atomic<int> reader_failures=0,reads=0,interleaved_reads=0,writes_started=0;
    std::vector<std::thread> readers;
    for (int i=0;i<3;++i) readers.emplace_back([&]() {
        do {
            const auto c=NpcCombo::Load("Concurrent");
            if (c.chain.size()!=1000||NpcCombo::Encode(NpcCombo::Decode(NpcCombo::Encode(c)))!=NpcCombo::Encode(c)) ++reader_failures;
            if (!done&&writes_started>0) ++interleaved_reads;
            ++reads; std::this_thread::yield();
        } while (!done);
    });
    for (int i=0;i<200;++i) { concurrent.chain.back().x=500+i; ++writes_started; NpcCombo::Save("Concurrent",concurrent); ++saves; if (i%5==0) Sleep(1); }
    done=true; for (auto& reader:readers) reader.join();
    Check(reader_failures==0&&interleaved_reads>=30,"three readers observe complete snapshots interleaved with 200 saves");
    Check(NpcCombo::SelectFile(L"concurrent")&&NpcCombo::Encode(NpcCombo::Load("Concurrent"))==NpcCombo::Encode(concurrent),"last concurrent save is on disk"); ++reloads;
    const auto good=Read(folder/L"concurrent.combo");
    const auto baseline=NpcCombo::Encode(NpcCombo::Load("Concurrent"));
    for (size_t cut:{0u,7u,23u}) {
        const std::vector<uint8_t> broken(good.begin(),good.begin()+(std::min)(cut,good.size()));
        Check(Write(folder/L"broken.combo",broken)&&!NpcCombo::SelectFile(L"broken")&&NpcCombo::Encode(NpcCombo::Load("Concurrent"))==baseline,"truncated file rejected without losing active nodes");
    }
    auto corrupt=good; corrupt[16]^=0x55; // CRC field; compressed padding bits need not change decoded data.
    Check(Write(folder/L"broken.combo",corrupt)&&!NpcCombo::SelectFile(L"broken")&&NpcCombo::Encode(NpcCombo::Load("Concurrent"))==baseline,"corrupt file rejected without losing active nodes");
    Check(!fs::exists(folder/L"default.combo"),"no default file created or overwritten even in isolated folder");
    Check(!fs::exists(folder/L"concurrent.combo.tmp"),"atomic write leaves no temporary file");
    const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
    std::printf("{\"checks\":%d,\"failures\":%d,\"saves\":%d,\"reloads\":%d,\"concurrent_reads\":%d,\"interleaved_reads\":%d,\"max_nodes_per_profile\":10000,\"large_profiles\":9,\"elapsed_ms\":%lld,\"isolated\":true}\n",checks,failures,saves,reloads,reads.load(),interleaved_reads.load(),elapsed);
    // Only exact files in the newly created private directory; no recursive delete.
    for (const auto& entry:fs::directory_iterator(folder)) if (entry.is_regular_file()) fs::remove(entry.path());
    fs::remove(folder); fs::remove(root/L"test.ini"); fs::remove(root);
    return failures?1:0;
}
