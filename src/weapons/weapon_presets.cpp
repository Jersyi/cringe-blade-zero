#include "weapon_presets.h"
#include "weapon_presets_logic.h"
#include "ui_hotkeys.h"
#include "ue_runtime.h"
#include "settings_store.h"
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <sstream>
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

namespace WeaponPresets {
namespace {
using ue::Object; using ue::Name; using ue::Call; using ue::Array;
using WeaponPresetsLogic::Step;
constexpr const char* kBackpack = "/Script/ProjectPBZ.PBZBackPackSubsystem";
SRWLOCK snapshot_lock = SRWLOCK_INIT, request_lock = SRWLOCK_INIT;
Snapshot snapshot;
Logger logger = nullptr; InputFocus input_focus = nullptr;
volatile LONG hid_allowed = 0, hid_ready = 0, hid_swipes = 0, hid_reports = 0, hid_error = 0;
HANDLE hid_thread = nullptr, hid_stop = nullptr;
int pending_enable = -1, pending_action = 0;
bool pending_read = false;
bool enabled = false, faulted = false, rollback_pending = false;
int active_set = 0;
Object* engine = nullptr;
struct Weak { int32_t index = -1, serial = 0; };
struct Identity { Object* object = nullptr; Name name; Weak weak; };
Identity cached_backpack, rollback_identity;
struct Set { bool known = false,bankable=true; std::vector<Name> main, shaqi; int32_t main_index = 0, shaqi_index = 0; };
Set sets[2], rollback;
Set current_equipment;
Object* rollback_backpack = nullptr; Name rollback_name;
struct Context { Object* world = nullptr, *instance = nullptr, *controller = nullptr, *pawn = nullptr, *backpack = nullptr, *weapons = nullptr; };
#ifdef PBZ_WEAPON_PRESETS_TEST
// Test-only native boundary. The fixture executes the production Tick/Save/Swap
// state machine; UE I/O is supplied separately and never enters a live process.
struct TestBackend {
    bool (*context)(Context&)=nullptr;
    bool (*capture)(Object*,Set&)=nullptr;
    bool (*restricted)(const Context&)=nullptr;
    bool (*plan)(Object*,const Set&,const Set&)=nullptr;
    bool (*apply)(Object*,const Set&)=nullptr;
    bool (*inventory)(Object*,bool)=nullptr;
    Object* (*row)(bool,Name)=nullptr;
    Object* (*soft)(Object*)=nullptr;
    bool (*paused)(const Context&,bool&)=nullptr;
};
TestBackend test_backend;
#endif
ULONGLONG next_publish = 0;
ULONGLONG next_read = 0;
ULONGLONG next_transaction_poll = 0;
std::wstring diagnostic;
std::wstring last_read_diagnostic;
bool last_runtime_special=false;

void Logf(const char* format, ...) {
    if (!logger) return;
    char text[800]{}; va_list args; va_start(args, format); vsprintf_s(text, format, args); va_end(args); logger(text);
}
void Status(const wchar_t* text) {
    AcquireSRWLockExclusive(&snapshot_lock); snapshot.status = text; ReleaseSRWLockExclusive(&snapshot_lock);
}
bool Diagnose(const wchar_t* format, ...) {
    wchar_t text[640]{}; va_list args; va_start(args, format); vswprintf_s(text, format, args); va_end(args);
    diagnostic = text; return false;
}
void DetailedStatus(const wchar_t* prefix) { const std::wstring text = std::wstring(prefix) + L"：" + diagnostic; Status(text.c_str()); }
bool Readable(const void* p, size_t size) {
    MEMORY_BASIC_INFORMATION info{};
    if (!p || !VirtualQuery(p, &info, sizeof(info)) || info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const uintptr_t at = reinterpret_cast<uintptr_t>(p), base = reinterpret_cast<uintptr_t>(info.BaseAddress);
    return at >= base && size <= info.RegionSize && at - base <= info.RegionSize - size;
}
bool Same(const Identity& id) {
#ifdef PBZ_WEAPON_PRESETS_TEST
    if(test_backend.context)return id.object!=nullptr&&id.weak.serial==1;
#endif
    auto* objects = reinterpret_cast<unsigned char*>(ue::Base() + ue::layout::kObjectArray);
    if (!id.object || id.weak.index < 0 || id.weak.serial <= 0 || !Readable(objects, 40) || id.weak.index >= ue::At<int32_t>(objects, 32)) return false;
    auto* chunks = ue::At<unsigned char**>(objects, 24); const size_t chunk = static_cast<uint32_t>(id.weak.index) >> 16;
    if (!Readable(chunks, (chunk + 1) * sizeof(void*)) || !chunks[chunk]) return false;
    auto* item = chunks[chunk] + (static_cast<uint32_t>(id.weak.index) & 0xffff) * 24ull;
    return Readable(item, 24) && ue::At<Object*>(item, 8) == id.object && ue::At<int32_t>(item, 16) == id.weak.serial &&
        ue::Alive(id.object) && ue::NameOf(id.object) == id.name;
}
Identity Identify(Object* object) {
#ifdef PBZ_WEAPON_PRESETS_TEST
    if(test_backend.context)return {object,Name{},Weak{1,1}};
#endif
    // Exact native FWeakObjectPtr assignment already validated by AirSkills and
    // MotianGrab. It allocates an object serial, never adds a strong GC hold.
    static constexpr unsigned char expected[] = {0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0x12,0x48,0x8b,0xd9,0x48,0x85,0xd2,0x74};
    if (!ue::Alive(object) || !ue::Base() || std::memcmp(reinterpret_cast<void*>(ue::Base() + 0x166f2a0), expected, sizeof(expected))) return {};
    Identity id; id.object = object; id.name = ue::NameOf(object);
    reinterpret_cast<void(__fastcall*)(Weak*,Object* const*)>(ue::Base() + 0x166f2a0)(&id.weak, &object);
    return Same(id) ? id : Identity{};
}
Object* Field(Object* cls, const char* name) {
    for (int supers = 0; cls && supers < 64; ++supers, cls = ue::At<Object*>(cls, 0x40)) {
        int count = 0;
        for (Object* p = ue::At<Object*>(cls, 0x50); p && ++count < 1024; p = ue::At<Object*>(p, 0x10))
            if (ue::NameIs(ue::At<Name>(p, 0x20), name)) return p;
    }
    return nullptr;
}
uint64_t Kind(Object* field) { return field ? ue::At<uint64_t>(ue::At<Object*>(field, 8), 0x18) : 0; }
bool Typed(Object* object, const char* name, uint64_t kind, int size, Object*& field) {
    field = ue::Alive(object) ? Field(ue::ClassOf(object), name) : nullptr;
    if (!ue::Alive(object)) return Diagnose(L"%hs：对象尚未就绪", name);
    if (!field) return Diagnose(L"缺少反射属性 %hs", name);
    const uint64_t actual_kind = Kind(field); const int actual_size = ue::At<int32_t>(field, 0x34), offset = ue::At<int32_t>(field, 0x44);
    if (actual_kind != kind || actual_size != size) return Diagnose(L"%hs 类型/大小不匹配（kind=%llx size=%d，期望 %llx/%d）", name, actual_kind, actual_size, kind, size);
    if (offset < 0 || !Readable(object + offset, static_cast<size_t>(size))) return Diagnose(L"%hs 数据不可读", name);
    return true;
}
template<class T> bool Read(Object* object, const char* name, uint64_t kind, T& value) {
    Object* field = nullptr;
    if (!Typed(object, name, kind, static_cast<int>(sizeof(T)), field)) return false;
    value = ue::At<T>(object, ue::At<int32_t>(field, 0x44)); return true;
}
Object* Property(Object* object, const char* name) { Object* result = nullptr; return Read(object, name, 0x10000, result) ? result : nullptr; }
bool IsWithin(Object* object, Object* owner) {
    for (int depth = 0; object && depth < 48; ++depth, object = ue::OuterOf(object)) if (object == owner) return true;
    return false;
}
Object* Component(Object* pawn, const char* path) {
    Object* type = ue::FindObject(path); Call call(pawn, "GetComponentByClass");
    return type && call.Set("ComponentClass", type).Invoke() ? call.Get<Object*>("ReturnValue") : nullptr;
}
bool ContextNow(Context& c) {
#ifdef PBZ_WEAPON_PRESETS_TEST
    if(test_backend.context)return test_backend.context(c);
#endif
    if (!ue::Alive(engine)) engine = ue::FindEngine();
    if (!ue::Alive(engine)) return Diagnose(L"GameEngine 尚未就绪");
    Object* viewport = Property(engine, "GameViewport"); c.world = Property(viewport, "World");
    c.instance = Property(c.world, "OwningGameInstance");
    Array players;
    if (!ue::Alive(c.world)) return Diagnose(L"GameViewport.World 尚未就绪");
    if (!Read(c.instance, "LocalPlayers", 0x200000, players)) return false;
    if (players.num < 1 || players.num > 16 || players.max < players.num || !Readable(players.data, static_cast<size_t>(players.num) * sizeof(void*)))
        return Diagnose(L"LocalPlayers 数组尚未就绪（%d/%d）", players.num, players.max);
    c.controller = Property(static_cast<Object**>(players.data)[0], "PlayerController"); c.pawn = Property(c.controller, "Pawn");
    if (!ue::Alive(c.pawn)) return Diagnose(L"本地 PlayerController.Pawn 尚未就绪");
    c.weapons = Component(c.pawn, "/Script/SGameCombatSystem.CharacterWeaponsComponent");
    Object* backpack_type = ue::FindObject(kBackpack);
    if (!backpack_type) return Diagnose(L"PBZBackPackSubsystem 反射类未找到");
    if (!ue::Alive(c.weapons)) return Diagnose(L"主角 CharacterWeaponsComponent 未找到");
    if (Same(cached_backpack) && ue::IsA(cached_backpack.object, backpack_type) && IsWithin(cached_backpack.object, c.world)) {
        c.backpack = cached_backpack.object; return true;
    }
    ue::ForEachObject([&](Object* object) {
        if (!ue::IsTemplate(object) && ue::IsA(object, backpack_type) && IsWithin(object, c.world)) {
            if (c.backpack) { c.backpack = nullptr; Diagnose(L"当前世界存在多个原生背包实例，未选择不明对象"); return false; }
            c.backpack = object;
        }
        return true;
    });
    if (ue::Alive(c.backpack)) { cached_backpack = Identify(c.backpack); return Same(cached_backpack) || Diagnose(L"原生背包 WeakObjectPtr 身份校验失败"); }
    return Diagnose(L"当前世界的 PBZBackPackSubsystem 尚未创建");
}
bool ArrayNames(Object* object, const char* name, std::vector<Name>& names) {
    Object* field = nullptr;
    if (!Typed(object, name, 0x200000, 16, field)) return false;
    Object* inner = ue::At<Object*>(field, 0x78);
    if (Kind(inner) != 0x2000 || ue::At<int32_t>(inner, 0x34) != 8) return Diagnose(L"%hs 数组元素不是 FName8", name);
    const Array array = ue::At<Array>(object, ue::At<int32_t>(field, 0x44));
    if (array.num < 1 || array.num > 2 || array.max < array.num || array.max > 1024 || !Readable(array.data, static_cast<size_t>(array.num) * sizeof(Name)))
        return Diagnose(L"%hs 槽位数组尚未稳定（%d/%d；当前支持1至2个已装备槽）", name, array.num, array.max);
    names.assign(static_cast<Name*>(array.data), static_cast<Name*>(array.data) + array.num);
    return WeaponPresetsLogic::ValidIds(names) || Diagnose(L"%hs 存在空ID或重复武器ID", name);
}
bool ConfigHelpersMatch() {
#ifdef PBZ_WEAPON_PRESETS_TEST
    if(test_backend.row&&test_backend.soft)return true;
    if(test_backend.capture&&test_backend.apply)return true;
#endif
    struct Signature { uintptr_t rva; unsigned char code[16]; };
    static constexpr Signature signatures[] = {
        {0x6955e10,{0x48,0x83,0xec,0x28,0x8b,0x0d,0xd6,0x6d,0x82,0x07,0x65,0x48,0x8b,0x04,0x25,0x58}},
        {0x6955d20,{0xe9,0xfb,0x97,0x39,0x13,0x0f,0x8e,0xfd,0x5d,0x57,0x04,0x0f,0x8b,0x26,0x64,0xd7}},
        {0x66304b0,{0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xec,0x20,0x48}},
        {0x1618f60,{0x40,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0xf9,0xe8,0x42,0xa1,0x05,0x00,0x48,0x85}},
        {0x5e772d0,{0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57}},
        {0x5e77320,{0x8b,0x43,0x10,0xeb,0x05,0xb8,0xff,0xff,0xff,0xff,0x48,0x8b,0x5c,0x24,0x30,0x48}}
        ,{0x694f800,{0x40,0x53,0x48,0x83,0xec,0x20,0x4c,0x8b,0xd2,0x41,0xb8,0xb9,0x79,0x37,0x9e,0x8b}}
    };
    if(!ue::Base())return Diagnose(L"原生武器数据入口尚未初始化");
    for(const auto& signature:signatures) if(std::memcmp(reinterpret_cast<void*>(ue::Base()+signature.rva),signature.code,sizeof(signature.code)))
        return Diagnose(L"原生武器数据入口签名不符（RVA=%llx）",static_cast<unsigned long long>(signature.rva));
    return true;
}
Object* WeaponRow(bool main,Name id) {
#ifdef PBZ_WEAPON_PRESETS_TEST
    if(test_backend.row)return test_backend.row(main,id);
#endif
    void* rows=reinterpret_cast<void*(__fastcall*)()>(ue::Base()+(main?0x6955e10:0x6955d20))();
    return rows?reinterpret_cast<Object*(__fastcall*)(void*,Name)>(ue::Base()+0x66304b0)(rows,id):nullptr;
}
Object* ResolveSoftConfig(Object* soft) {
#ifdef PBZ_WEAPON_PRESETS_TEST
    if(test_backend.soft)return test_backend.soft(soft);
#endif
    // Native FSoftObjectPtr::Get resolves only an already-loaded UObject. It
    // does not request an async load or LoadSynchronous during a UI refresh.
    return reinterpret_cast<Object*(__fastcall*)(Object*)>(ue::Base()+0x1618f60)(soft);
}
bool ExpectedConfigs(Name id,bool main,Object*& normal,Object*& snap) {
    Object* type=ue::FindObject(main?"/Script/ProjectPBZ.WeaponData":"/Script/ProjectPBZ.ShaqiWeaponData");
    Object* config=Field(type,"WeaponConfig"),*soul=Field(type,main?"WeaponSnapSoulData":"ShaChiWeaponSnapSoulData");
    if(!type||ue::StructSize(type)!=(main?1880:2096)||!config||Kind(config)!=0x20000000||ue::At<int32_t>(config,0x34)!=40||
        !soul||Kind(soul)!=0x100000)return Diagnose(L"WeaponData/ShaqiWeaponData 配置行布局校验失败");
    Object* soul_type=ue::At<Object*>(soul,0x70),*soul_config=Field(soul_type,"WeaponConfig");
    if(!soul_config||Kind(soul_config)!=0x20000000||ue::At<int32_t>(soul_config,0x34)!=40)return Diagnose(L"武器吸魂配置行布局校验失败");
    const int32_t base_at=ue::At<int32_t>(config,0x44),snap_at=ue::At<int32_t>(soul,0x44)+ue::At<int32_t>(soul_config,0x44);
    if(base_at!=(main?0x520:0x5f8)||snap_at!=(main?0x3f0:0x470))return Diagnose(L"武器配置行偏移与原生配置身份查找路径不符");
    Object* row=WeaponRow(main,id);
    if(!row||!Readable(row,static_cast<size_t>(ue::StructSize(type))))return Diagnose(L"目标武器ID没有可读的原生配置表行");
    normal=ResolveSoftConfig(row+base_at);snap=ResolveSoftConfig(row+snap_at);
    return true;
}
bool ConfigRequests(Object* backpack,int& pending) {
    pending=0;
#ifdef PBZ_WEAPON_PRESETS_TEST
    if(test_backend.capture&&test_backend.apply)return true; // Whole-native-state fixture; typed fixture leaves Capture seam null.
#endif
    // Exact PE ConfigReadyDispatch693e31a + Find694f800: native TMap at
    // backpack+c70, sparse stride48, key{Shaqi:byte0,Index:int4}, value32@8.
    // Callback checks value.Generation@8, removes the row BEFORE broadcasting
    // ConfigReady. A timeout by itself never proves the callback has retired.
    if(!ue::Alive(backpack)||ue::StructSize(ue::ClassOf(backpack))<0xcc0||!Readable(backpack+0xc70,80))return Diagnose(L"原生装备加载请求表不可读");
    auto* map=backpack+0xc70;
    const int32_t count=ue::At<int32_t>(map,8),capacity=ue::At<int32_t>(map,12),bits=ue::At<int32_t>(map,40),max_bits=ue::At<int32_t>(map,44),free=ue::At<int32_t>(map,52);
    if(count<0||count>64||capacity<count||bits!=count||max_bits<bits||free<0||free>count)return Diagnose(L"原生装备加载请求表尚未稳定");
    if(!count)return true;
    auto* data=ue::At<unsigned char*>(map,0);auto* allocation=ue::At<uint32_t*>(map,32);if(!allocation)allocation=reinterpret_cast<uint32_t*>(map+16);
    if(!Readable(data,static_cast<size_t>(count)*48)||!Readable(allocation,(static_cast<size_t>(bits)+31)/32*4))return Diagnose(L"原生装备加载请求内容不可读");
    std::string fingerprint;
    for(int32_t i=0;i<count;++i)if(allocation[i/32]&(1u<<(i%32))){
        auto* row=data+static_cast<size_t>(i)*48;const uint8_t shaqi=ue::At<uint8_t>(row,0);const int32_t index=ue::At<int32_t>(row,4);
        if(shaqi>1||index<0||index>1)return Diagnose(L"原生装备加载请求键与已审计的两槽布局不同");
        const uint64_t generation=ue::At<uint64_t>(row,16);++pending;
        char item[100]{};sprintf_s(item,"%s%d:g%llu;",shaqi?"Shaqi":"Main",index,static_cast<unsigned long long>(generation));fingerprint+=item;
    }
    static std::string last;
    if(fingerprint!=last){last=fingerprint;Logf("WEAPON_PRESETS native config jobs count=%d keys=%s",pending,fingerprint.c_str());}
    return true;
}
struct SelectionProof { int32_t committed=-1,pending=-1; Object* selected_config=nullptr; Name tag; bool special=false; };
bool RuntimeSelection(Object* component,bool main,const std::vector<Name>& ids,SelectionProof& proof) {
    static std::string last_mismatch[2];
    const char* slot_name=main?"Weapon.Slot.Main":"Weapon.Slot.Shachi";
    Object* map_property=nullptr;
    if(!Typed(component,"RuntimeWeaponData",0x400000000000ull,80,map_property))return false;
    Object* key=ue::At<Object*>(map_property,0x70),*value=ue::At<Object*>(map_property,0x78);
    Object* slot_type=ue::FindObject("/Script/SGameCombatSystem.RuntimeWeaponSlotData"),*single_type=ue::FindObject("/Script/SGameCombatSystem.RuntimeWeaponSingleData");
    Object* weapons=Field(slot_type,"Weapons"),*config=Field(single_type,"WeaponConfig");
    if(Kind(key)!=0x100000||ue::At<int32_t>(key,0x34)!=8||Kind(value)!=0x100000||ue::At<int32_t>(value,0x34)!=32||
        ue::At<Object*>(value,0x70)!=slot_type||!slot_type||ue::StructSize(slot_type)!=32||!single_type||ue::StructSize(single_type)!=288||
        !weapons||Kind(weapons)!=0x200000||ue::At<int32_t>(weapons,0x34)!=16||ue::At<int32_t>(weapons,0x44)!=0||
        !config||Kind(config)!=0x1000||ue::At<int32_t>(config,0x34)!=16||ue::At<int32_t>(config,0x44)!=0)
        return Diagnose(L"RuntimeWeaponData 武器槽/配置接口布局校验失败");
    Object* inner=ue::At<Object*>(weapons,0x78);
    if(Kind(inner)!=0x100000||ue::At<int32_t>(inner,0x34)!=288||ue::At<Object*>(inner,0x70)!=single_type)return Diagnose(L"RuntimeWeaponData.Weapons 数组元素布局不符");
    unsigned char* map=component+ue::At<int32_t>(map_property,0x44);
    const int32_t count=ue::At<int32_t>(map,8),capacity=ue::At<int32_t>(map,12),bits=ue::At<int32_t>(map,40),max_bits=ue::At<int32_t>(map,44),free=ue::At<int32_t>(map,52);
    const int32_t stride=ue::At<int32_t>(map_property,0x8c),offset=ue::At<int32_t>(map_property,0x80);
    if(count<1||count>64||capacity<count||bits!=count||max_bits<bits||free<0||free>count||stride<40||stride>256||offset<8||offset+32>stride)
        return Diagnose(L"RuntimeWeaponData 稀疏Map尚未稳定");
    auto* data=ue::At<unsigned char*>(map,0);auto* allocation=ue::At<uint32_t*>(map,32);
    if(!allocation)allocation=reinterpret_cast<uint32_t*>(map+16);
    if(!Readable(data,static_cast<size_t>(count)*stride)||!Readable(allocation,(static_cast<size_t>(bits)+31)/32*4))return Diagnose(L"RuntimeWeaponData 数据不可读");
    unsigned char* found=nullptr;
    for(int32_t i=0;i<count;++i)if(allocation[i/32]&(1u<<(i%32))) {
        auto* row=data+static_cast<size_t>(i)*stride;Name tag=ue::At<Name>(row,0);
        if(ue::NameIs(tag,slot_name)) {if(found)return Diagnose(L"RuntimeWeaponData 包含重复的原生武器槽");found=row+offset;proof.tag=tag;}
    }
    if(!found)return Diagnose(L"RuntimeWeaponData 缺少 %hs",slot_name);
    const Array array=ue::At<Array>(found,0);
    // BP_RenewalPlayer registers canonical equipment at the same index. Main
    // additionally has a special index2; its switch callback intentionally
    // excludes that slot from the backpack's normal-weapon selection cache.
    const bool count_ok=array.num==static_cast<int32_t>(ids.size())||(main&&array.num==3);
    if(!count_ok||array.max<array.num||array.max>1024||!Readable(array.data,static_cast<size_t>(array.num)*288))
        return Diagnose(L"%hs 原生配置数量与装备ID列表不同（%d/%d），等待配置完成",slot_name,array.num,static_cast<int>(ids.size()));
    Call committed(component,"GetCurrentWeaponIndex"),pending(component,"GetCurrentWeaponIndex");
    if(!committed.Slot("ReturnValue",4)||!committed.Set("SlotTag",proof.tag).Set("WithPending",false).Invoke()||
        !pending.Slot("ReturnValue",4)||!pending.Set("SlotTag",proof.tag).Set("WithPending",true).Invoke())return Diagnose(L"%hs 原生已提交/待切换索引读取失败",slot_name);
    proof.committed=committed.Get<int32_t>("ReturnValue");proof.pending=pending.Get<int32_t>("ReturnValue");
    if(proof.committed!=ue::At<int32_t>(found,16))return Diagnose(L"%hs 原生已提交索引读回与运行时槽不一致",slot_name);
    if(proof.committed<0||proof.committed>=array.num)return Diagnose(L"%hs 已提交索引不在配置数组中（%d）",slot_name,proof.committed);
    proof.selected_config=ue::At<Object*>(static_cast<unsigned char*>(array.data)+static_cast<size_t>(proof.committed)*288,0);
    if(proof.pending!=proof.committed)return Diagnose(L"%hs 仍在等待原生切换提交（%d→%d）",slot_name,proof.committed,proof.pending);
    proof.special=static_cast<size_t>(proof.committed)>=ids.size();
    if(proof.special&&!ue::Alive(proof.selected_config))return Diagnose(L"%hs 额外武器槽的配置对象尚未就绪",slot_name);
    for(int32_t i=0;i<static_cast<int32_t>(ids.size());++i) {
        Object* actual=ue::At<Object*>(static_cast<unsigned char*>(array.data)+static_cast<size_t>(i)*288,0),*normal=nullptr,*snap=nullptr;
        if(!ue::Alive(actual))return Diagnose(L"%hs 配置对象尚未就绪（槽位=%d）",slot_name,i);
        if(!ExpectedConfigs(ids[static_cast<size_t>(i)],main,normal,snap))return false;
        if(actual!=normal&&actual!=snap) {
            char id[120]{},actual_name[120]{};ue::NameText(ids[static_cast<size_t>(i)],id,sizeof(id));ue::NameText(ue::NameOf(actual),actual_name,sizeof(actual_name));
            char state[640]{};sprintf_s(state,"slot=%s index=%d id=%s raw=%u/%u actual=%s ptr=%p expected_normal=%p expected_snap=%p",slot_name,i,id,ids[static_cast<size_t>(i)].index,ids[static_cast<size_t>(i)].number,actual_name,static_cast<void*>(actual),static_cast<void*>(normal),static_cast<void*>(snap));
            if(last_mismatch[main?0:1]!=state){last_mismatch[main?0:1]=state;Logf("WEAPON_PRESETS config mismatch %s",state);}
            return Diagnose(L"%hs 配置对象与武器ID不匹配（槽位=%d），等待原生装备重建",slot_name,i);
        }
        if(i==proof.committed)proof.selected_config=actual;
    }
    last_mismatch[main?0:1].clear();
    return true;
}
bool Capture(Object* backpack, Set& output) {
#ifdef PBZ_WEAPON_PRESETS_TEST
    if(test_backend.capture){const bool ok=test_backend.capture(backpack,output);last_runtime_special=ok&&!output.bankable;return ok;}
#endif
    Set result;int32_t cached_main=-1,cached_sha=-1;Name cached_id;
    if (!ArrayNames(backpack, "MainWeapons", result.main) || !ArrayNames(backpack, "ShaqiWeapons", result.shaqi) ||
        !Read(backpack, "CurrentWeaponIndex", 0x80, cached_main) || !Read(backpack, "CurrentShaqiWeaponIndex", 0x80, cached_sha)||
        !Read(backpack,"CurrentMainWeaponID",0x2000,cached_id)) return false;
    Context current;
    if(!ContextNow(current)||current.backpack!=backpack)return Diagnose(L"读取装备时本地玩家/背包已改变");
    if(!ConfigHelpersMatch())return false;
    int jobs=0;if(!ConfigRequests(backpack,jobs))return false;
    if(jobs)return Diagnose(L"原生装备资源加载请求尚未退休（%d项），等待读取已提交配置",jobs);
    Call transition(current.weapons,"IsInWeaponTransition");
    if(!transition.Slot("ReturnValue",1)||!transition.Invoke())return Diagnose(L"IsInWeaponTransition 原生接口读取失败");
    if(transition.Get<bool>("ReturnValue"))return Diagnose(L"正在等待原生武器配置/切换提交");
    SelectionProof main,sha;
    const bool main_ready=RuntimeSelection(current.weapons,true,result.main,main);
    const std::wstring main_error=diagnostic;
    const bool sha_ready=RuntimeSelection(current.weapons,false,result.shaqi,sha);
    last_runtime_special=main.special||sha.special;
    static std::string last_raw;
    char cache_name[100]{},id_name[100]{};ue::NameText(cached_id,cache_name,sizeof(cache_name));
    if(main.committed>=0&&static_cast<size_t>(main.committed)<result.main.size())ue::NameText(result.main[static_cast<size_t>(main.committed)],id_name,sizeof(id_name));
    char main_config[80]{},sha_config[80]{};if(ue::Alive(main.selected_config))ue::NameText(ue::NameOf(main.selected_config),main_config,sizeof(main_config));if(ue::Alive(sha.selected_config))ue::NameText(ue::NameOf(sha.selected_config),sha_config,sizeof(sha_config));
    char raw[720]{};sprintf_s(raw,"cache_id=%s(%u/%u) cache_selection=%d/%d runtime_committed=%d/%d runtime_pending=%d/%d main_id=%s main_config=%p:%s shaqi_config=%p:%s ready=%d/%d",cache_name,cached_id.index,cached_id.number,cached_main,cached_sha,main.committed,sha.committed,main.pending,sha.pending,id_name,static_cast<void*>(main.selected_config),main_config,static_cast<void*>(sha.selected_config),sha_config,main_ready?1:0,sha_ready?1:0);
    if(last_raw!=raw){last_raw=raw;Logf("WEAPON_PRESETS capture %s",raw);}
    if(!main_ready){diagnostic=main_error;return false;}if(!sha_ready)return false;
    // CurrentMainWeaponID is a gameplay/backpack cache. Replacing the currently
    // equipped weapon can leave it referring to the previous ID even after the
    // actual runtime slot/config is committed. Do not use that cache as truth.
    result.main_index=main.committed;result.shaqi_index=sha.committed;
    result.bankable=!last_runtime_special;
    result.known = true; output = std::move(result); return true;
}
bool Equal(const Set& a, const Set& b) {
    return a.known && b.known && a.bankable==b.bankable && a.main == b.main && a.shaqi == b.shaqi && a.main_index == b.main_index && a.shaqi_index == b.shaqi_index;
}
struct OwnedInfo { Name id; Object* object = nullptr; int32_t index = -1; Name object_name; int32_t level = 0; };
std::vector<OwnedInfo> rollback_main, rollback_sha;
uint32_t rollback_attempts = 0;
ULONGLONG next_rollback = 0;
bool Inventory(Object* backpack, bool main, std::vector<OwnedInfo>& items) {
    Object* field = nullptr;
    if (!Typed(backpack, main ? "MainWeaponInfos" : "ShaqiWeaponInfos", 0x400000000000ull, 80, field)) return false;
    Object* key = ue::At<Object*>(field, 0x70), *value = ue::At<Object*>(field, 0x78);
    if (Kind(key) != 0x2000 || Kind(value) != 0x10000 || ue::At<int32_t>(key, 0x34) != 8 || ue::At<int32_t>(value, 0x34) != 8)
        return Diagnose(L"%hs Map键/值布局不匹配", main ? "MainWeaponInfos" : "ShaqiWeaponInfos");
    unsigned char* map = backpack + ue::At<int32_t>(field, 0x44);
    const int32_t count = ue::At<int32_t>(map, 8), capacity = ue::At<int32_t>(map, 12), bits = ue::At<int32_t>(map, 40),
        max_bits = ue::At<int32_t>(map, 44), free = ue::At<int32_t>(map, 52), stride = ue::At<int32_t>(field, 0x8c), offset = ue::At<int32_t>(field, 0x80);
    if (count < 1 || count > 512 || capacity < count || bits != count || max_bits < bits || free < 0 || free > count || stride < 16 || stride > 256 || offset < 8 || offset + 8 > stride)
        return Diagnose(L"%hs Map布局尚未稳定（count=%d stride=%d）", main ? "MainWeaponInfos" : "ShaqiWeaponInfos", count, stride);
    auto* data = ue::At<unsigned char*>(map, 0); auto* allocation = ue::At<uint32_t*>(map, 32);
    if (!allocation) allocation = reinterpret_cast<uint32_t*>(map + 16);
    if (!Readable(data, static_cast<size_t>(count) * stride) || !Readable(allocation, (static_cast<size_t>(bits) + 31) / 32 * 4)) return Diagnose(L"武器库存 Map 数据不可读");
    items.clear();
    for (int32_t i = 0; i < count; ++i) if (allocation[i / 32] & (1u << (i % 32))) {
        unsigned char* row = data + static_cast<size_t>(i) * stride;
        OwnedInfo item; item.id = ue::At<Name>(row, 0); item.object = ue::At<Object*>(row, offset);
        if (item.id.None() || !ue::Alive(item.object)) return Diagnose(L"武器库存 Map 含未就绪的 Info 对象");
        if (!Read(item.object, main ? "WeaponLevel" : "ShaChiWeaponLevel", 0x80, item.level)) return false;
        item.index = ue::At<int32_t>(item.object, ue::layout::kObjIndex); item.object_name = ue::NameOf(item.object); items.push_back(item);
    }
    return items.size() == static_cast<size_t>(count - free) || Diagnose(L"武器库存稀疏 Map 计数不一致");
}
bool SameInventory(Object* backpack, bool main, const std::vector<OwnedInfo>& before) {
#ifdef PBZ_WEAPON_PRESETS_TEST
    if(test_backend.inventory)return test_backend.inventory(backpack,main);
#endif
    std::vector<OwnedInfo> after;
    if (!Inventory(backpack, main, after)) return false;
    if (before.size() != after.size()) return Diagnose(L"装备切换后库存数量发生变化，已请求恢复");
    for (const auto& item : before) {
        auto found = std::find_if(after.begin(), after.end(), [&](const OwnedInfo& other) { return other.id == item.id; });
        if (found == after.end() || found->object != item.object || found->index != item.index || !(found->object_name == item.object_name) || found->level != item.level)
            return Diagnose(L"装备切换后库存对象/等级发生变化，已请求恢复");
    }
    return true;
}
bool HasOwned(Object* backpack, Name id, bool main) {
    Call has(backpack, "HasWeapon"), locked(backpack, "IsWeaponLocked");
    char weapon[160]{}; ue::NameText(id,weapon,sizeof(weapon));
    if (!has.Slot("ReturnValue",1) || !has.Set("WeaponID", id).Set("bIsNormalWeapon", main).Invoke()) return Diagnose(L"HasWeapon 原生接口读取失败：%hs",weapon);
    if (!has.Get<bool>("ReturnValue")) return Diagnose(L"目标武器尚未拥有：%hs",weapon);
    if (!locked.Slot("ReturnValue",1) || !locked.Set("WeaponID", id).Set("IsNormalWeapon", main).Invoke()) return Diagnose(L"IsWeaponLocked 原生接口读取失败：%hs",weapon);
    return !locked.Get<bool>("ReturnValue") || Diagnose(L"目标武器被游戏锁定：%hs",weapon);
}
bool Family(Name tag, const char* prefix) {
    char text[256]{}; ue::NameText(tag, text, sizeof(text)); const size_t n = std::strlen(prefix);
    return !_strnicmp(text, prefix, n) && (!text[n] || text[n] == '.');
}
bool SafePlayerState(const Context& c) {
    Call dead(ue::Default("/Script/SGameAI.Default__SGameAIStateUtils"), "IsDead");
    if (!dead.Slot("ReturnValue",1) || !dead.Set("Pawn", c.pawn).Invoke()) return Diagnose(L"IsDead 生命状态接口读取失败");
    if (dead.Get<bool>("ReturnValue")) return Diagnose(L"主角处于死亡状态");
    Object* attributes = Property(c.pawn, "AttributeSetBase"), *data = ue::FindObject("/Script/GameplayAbilities.GameplayAttributeData");
    Object* health = nullptr;
    if (!Typed(attributes, "Health", 0x100000, 16, health) || !data || ue::At<Object*>(health, 0x70) != data ||
        ue::Offset(data, "CurrentValue") != 12 || ue::StructSize(data) != 16) return Diagnose(L"Health GameplayAttributeData 布局校验失败");
    const float hp = ue::At<float>(attributes, ue::At<int32_t>(health, 0x44) + 12);
    if (!std::isfinite(hp) || hp <= 0) return Diagnose(L"主角当前生命值不允许变更装备（%.2f）",hp);
    struct Tags { Array tags, parents; };
    const auto array_ok = [](Array array, size_t stride, int32_t limit = 1024) {
        return array.num >= 0 && array.num <= array.max && array.max <= limit && (!array.num || Readable(array.data, static_cast<size_t>(array.num) * stride));
    };
    Call states(Component(c.pawn, "/Script/SGameActorState.ActorStateComponent"), "GetCurrentStatesContainer");
    if (!states.Slot("CurrentStates", 32) || !states.Invoke()) return Diagnose(L"ActorState.GetCurrentStatesContainer 读取失败");
    const Tags tags = states.Get<Tags>("CurrentStates");
    bool safe = array_ok(tags.tags, 8) && array_ok(tags.parents, 8);
    constexpr const char* blocked[] = {"CharacterState.Dead", "CharacterState.FallDown", "CharacterState.HitReact",
        "CharacterState.Narrative", "CharacterState.Native.Sequence", "CharacterState.SpecialMove", "CharacterState.StoryCameraLock",
        "CharacterState.InVolume.SpecialMove", "CharacterState.Stand.Invulnerable.Execution", "CharacterState.Immortal.Execution",
        "CharacterState.Forbid.Ability.Focusing.Execution"};
    if (safe) for (int32_t i = 0; i < tags.tags.num; ++i) for (const char* prefix : blocked)
        if (Family(static_cast<Name*>(tags.tags.data)[i], prefix)) { char name[200]{}; ue::NameText(static_cast<Name*>(tags.tags.data)[i],name,sizeof(name)); Diagnose(L"主角状态阻止装备切换：%hs",name); safe = false; }
    ue::Free(tags.tags.data); ue::Free(tags.parents.data);
    if (!safe) return false;
    Call get(ue::Default("/Script/GameplayAbilities.Default__AbilitySystemBlueprintLibrary"), "GetAbilitySystemComponent");
    if (!get.Set("Actor", c.pawn).Invoke()) return Diagnose(L"GetAbilitySystemComponent 读取失败");
    Object* asc = get.Get<Object*>("ReturnValue");
    Object* spec = ue::FindObject("/Script/GameplayAbilities.GameplayAbilitySpec"), *container = ue::FindObject("/Script/GameplayAbilities.GameplayAbilitySpecContainer");
    Object* field = nullptr;
    if (!Typed(asc, "ActivatableAbilities", 0x100000, 288, field) || !spec || !container || ue::At<Object*>(field, 0x70) != container ||
        ue::StructSize(spec) != 240 || ue::Offset(container, "Items") != 264 || ue::Offset(spec, "ActiveCount") != 40 || ue::Offset(spec, "Ability") != 16) return Diagnose(L"ActivatableAbilities/GameplayAbilitySpec 布局校验失败");
    const Array specs = ue::At<Array>(asc, ue::At<int32_t>(field, 0x44) + 264);
    if (!array_ok(specs, 240)) return Diagnose(L"ActivatableAbilities.Items 数组尚未稳定");
    for (int32_t i = 0; i < specs.num; ++i) {
        auto* row = static_cast<unsigned char*>(specs.data) + static_cast<size_t>(i) * 240;
        if (!ue::At<uint8_t>(row, 40)) continue;
        Object* ability = ue::At<Object*>(row, 16); Tags ability_tags;
        if (!Read(ability, "AbilityTags", 0x100000, ability_tags)) return false;
        if (!array_ok(ability_tags.tags, 8)) return Diagnose(L"当前活跃技能的 AbilityTags 数组不可读");
        for (int32_t j = 0; j < ability_tags.tags.num; ++j) {
            const Name tag = static_cast<Name*>(ability_tags.tags.data)[j];
            if (Family(tag, "Ability.Action.Execution") || Family(tag, "Ability.Executed")) return Diagnose(L"主角正在执行或被执行，暂不变更装备");
        }
    }
    return true;
}
bool Restricted(const Context& c) {
#ifdef PBZ_WEAPON_PRESETS_TEST
    if(test_backend.restricted)return test_backend.restricted(c);
#endif
    if (!SafePlayerState(c)) return true;
    Object* bare = nullptr;
    if (!Typed(c.backpack, "bZlgwBareMode", 0x20000, 1, bare)) return true;
    const auto* layout = bare + 0x70;
    const int32_t offset = ue::At<int32_t>(bare, 0x44) + layout[1];
    if (ue::At<uint8_t>(c.backpack, offset) & layout[3]) { Diagnose(L"bZlgwBareMode 强制裸装模式阻止切换"); return true; }
    Call forced(c.backpack, "HasForcedWeaponLoadout"), transition(c.weapons, "IsInWeaponTransition"), cinematic(c.controller, "IsInCinematicMode");
    if (!forced.Slot("ReturnValue",1) || !forced.Invoke()) { Diagnose(L"HasForcedWeaponLoadout 原生接口读取失败"); return true; }
    if (forced.Get<bool>("ReturnValue")) { Diagnose(L"HasForcedWeaponLoadout：游戏正在强制使用指定装备"); return true; }
    if (!transition.Slot("ReturnValue",1) || !transition.Invoke()) { Diagnose(L"IsInWeaponTransition 原生接口读取失败"); return true; }
    if (transition.Get<bool>("ReturnValue")) { Diagnose(L"IsInWeaponTransition：原生武器切换尚未结束"); return true; }
    // Some PlayerController revisions do not expose a getter; reflected native
    // bCinematicMode still gates equipment changes while sequencer owns control.
    if (cinematic.Ok()) { if (!cinematic.Invoke() || cinematic.Get<bool>("ReturnValue")) { Diagnose(L"IsInCinematicMode：过场控制中"); return true; } }
    Object* cinematic_field = nullptr;
    if (Typed(c.controller, "bCinematicMode", 0x20000, 1, cinematic_field)) {
        const auto* flags = cinematic_field + 0x70;
        if (ue::At<uint8_t>(c.controller, ue::At<int32_t>(cinematic_field, 0x44) + flags[1]) & flags[3]) { Diagnose(L"bCinematicMode：原生过场控制中"); return true; }
    }
    return false;
}
bool BuildPlan(Object* backpack, const Set& from, const Set& to, std::vector<Step<Name>>& plan,
    std::vector<OwnedInfo>& main, std::vector<OwnedInfo>& shaqi) {
#ifdef PBZ_WEAPON_PRESETS_TEST
    if(test_backend.plan)return test_backend.plan(backpack,from,to);
#endif
    if (!to.known) return Diagnose(L"目标预设尚未保存，请先保存当前装备到该预设");
    if (from.main.size()!=to.main.size() || from.shaqi.size()!=to.shaqi.size()) return Diagnose(L"目标预设与当前装备的普通/杀气槽位数量不同，未修改装备");
    if (!Inventory(backpack, true, main) || !Inventory(backpack, false, shaqi)) return false;
    std::vector<Name> main_ids, shaqi_ids;
    for (const auto& info : main) main_ids.push_back(info.id);
    for (const auto& info : shaqi) shaqi_ids.push_back(info.id);
    // Validate targets and any cycle-breaking spare only. Calling inventory
    // predicates for every owned weapon would put an avoidable scan on a swipe.
    for (Name id : to.main) if (!HasOwned(backpack, id, true)) return false;
    for (Name id : to.shaqi) if (!HasOwned(backpack, id, false)) return false;
    for (size_t attempt = 0; attempt <= shaqi.size(); ++attempt) {
        plan.clear();
        if (!WeaponPresetsLogic::Plan(from.main, to.main, main_ids, true, plan) ||
            !WeaponPresetsLogic::Plan(from.shaqi, to.shaqi, shaqi_ids, false, plan)) return Diagnose(L"目标武器槽位无法通过原生替换完成；杀气武器对调需一把已拥有的空闲武器");
        bool removed = false;
        for (const auto& step : plan) if (!step.main && std::find(to.shaqi.begin(), to.shaqi.end(), step.id) == to.shaqi.end() && !HasOwned(backpack, step.id, false)) {
            shaqi_ids.erase(std::remove(shaqi_ids.begin(), shaqi_ids.end(), step.id), shaqi_ids.end()); removed = true; break;
        }
        if (!removed) return true;
    }
    return Diagnose(L"未找到可用于杀气武器对调的未锁定空闲武器");
}
bool ReadLayout(Object* backpack,Set& layout);
bool SubmitEquip(Object* backpack, const Set& target, bool restoring=false) {
#ifdef PBZ_WEAPON_PRESETS_TEST
    if(test_backend.apply)return test_backend.apply(backpack,target);
#endif
    Set from;
    std::vector<Step<Name>> plan; std::vector<OwnedInfo> main, shaqi;
    if(!Capture(backpack,from)&&(!restoring||!ReadLayout(backpack,from)))return false;
    if(!BuildPlan(backpack, from, target, plan, main, shaqi))return false;
    for (const auto& step : plan) {
        Call replace(backpack, step.main ? "ReplaceWeaponByIndex" : "ReplaceShaqiWeaponByIndex");
        if (!replace.Set(step.main ? "NewWeaponName" : "NewName", step.id).Set("ReplaceIndex", step.index).Invoke()) return Diagnose(L"%hs 原生调用失败，槽位=%d",step.main?"ReplaceWeaponByIndex":"ReplaceShaqiWeaponByIndex",step.index);
        std::vector<Name> result;
        if (!ArrayNames(backpack, step.main ? "MainWeapons" : "ShaqiWeapons", result) ||
            step.index < 0 || static_cast<size_t>(step.index) >= result.size() || !(result[static_cast<size_t>(step.index)] == step.id)) return Diagnose(L"%hs 原生替换未在槽位%d读回目标ID",step.main?"MainWeapons":"ShaqiWeapons",step.index);
        if (!SameInventory(backpack, true, main) || !SameInventory(backpack, false, shaqi)) return false;
    }
    // Replace may have queued StreamableManager work. Native request acceptance
    // is not completion; the owner waits for each config request to retire and
    // every runtime config to match before requesting the selected indices.
    return SameInventory(backpack, true, main) && SameInventory(backpack, false, shaqi);
}
bool SelectIndices(const Context& c,const Set& target) {
#ifdef PBZ_WEAPON_PRESETS_TEST
    if(test_backend.apply)return true; // Existing state-flow native-I/O seam applies the whole target.
#endif
    SelectionProof main,sha;
    if(!RuntimeSelection(c.weapons,true,target.main,main)||!RuntimeSelection(c.weapons,false,target.shaqi,sha))return false;
    Call normal(c.backpack,"SetCurrentWeaponByIndex"),ultimate(c.backpack,"SetCurrentWeaponByIndex");
    if(!normal.Set("WeaponIndex",target.main_index).Set("IsNormalWeapon",true).Invoke()||
        !ultimate.Set("WeaponIndex",target.shaqi_index).Set("IsNormalWeapon",false).Invoke())return Diagnose(L"SetCurrentWeaponByIndex 当前武器选择调用失败");
    for(const auto& choice:{std::make_pair(main.tag,target.main_index),std::make_pair(sha.tag,target.shaqi_index)}){
        Call actual(c.weapons,"GetCurrentWeaponIndex");
        if(!actual.Set("SlotTag",choice.first).Set("WithPending",false).Invoke())return Diagnose(L"原生实际武器选择读取失败");
        if(actual.Get<int32_t>("ReturnValue")==choice.second)continue;
        Call select(c.weapons,"SetCurrentWeapon");
        if(!select.Slot("ReturnValue",1)||!select.Set("SlotTag",choice.first).Set("Index",choice.second).Invoke()||!select.Get<bool>("ReturnValue"))
            return Diagnose(L"CharacterWeaponsComponent.SetCurrentWeapon 未接受目标索引（%d）",choice.second);
    }
    return true;
}
std::wstring Label(Name name) {
    char text[256]{}; ue::NameText(name, text, sizeof(text));
    const int size = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (size <= 1) return L"?";
    std::wstring result(static_cast<size_t>(size), L'\0'); MultiByteToWideChar(CP_UTF8, 0, text, -1, result.data(), size); result.resize(static_cast<size_t>(size - 1)); return result;
}
PresetView View(const Set& source) {
    PresetView view; view.known = source.known; view.special_slot=!source.bankable; view.main_index = source.main_index; view.shaqi_index = source.shaqi_index;
    for (Name id : source.main) view.main_weapons.push_back(Label(id));
    for (Name id : source.shaqi) view.shaqi_weapons.push_back(Label(id));
    return view;
}
// PBZDebugMenu.ini [WeaponPresets]: both banks by native weapon ID text, the
// active bank and the feature switch, so presets survive a game restart.
// Format per bank: bankable|main_index|shaqi_index|main,ids|shaqi,ids.
bool store_loaded = false;
std::wstring Encode(const Set& set) {
    if (!set.known) return L"";
    std::wstring text = std::to_wstring(set.bankable ? 1 : 0) + L"|" + std::to_wstring(set.main_index) + L"|" + std::to_wstring(set.shaqi_index) + L"|";
    for (size_t i = 0; i < set.main.size(); ++i) { if (i) text += L","; text += Label(set.main[i]); }
    text += L"|";
    for (size_t i = 0; i < set.shaqi.size(); ++i) { if (i) text += L","; text += Label(set.shaqi[i]); }
    return text;
}
std::vector<std::wstring> Split(const std::wstring& text, wchar_t separator) {
    std::vector<std::wstring> parts; size_t start = 0;
    for (;;) { const size_t at = text.find(separator, start); parts.push_back(text.substr(start, at == std::wstring::npos ? std::wstring::npos : at - start)); if (at == std::wstring::npos) break; start = at + 1; }
    return parts;
}
bool Decode(const std::wstring& text, Set& set) {
    const auto parts = Split(text, L'|');
    if (parts.size() != 5) return false;
    Set result; result.known = true; result.bankable = parts[0] == L"1";
    result.main_index = _wtoi(parts[1].c_str()); result.shaqi_index = _wtoi(parts[2].c_str());
    for (int list = 0; list < 2; ++list) for (const std::wstring& id : Split(parts[3 + list], L',')) {
        if (id.empty() || id == L"?") return false;
#ifdef PBZ_WEAPON_PRESETS_TEST
        const Name name{};
#else
        const Name name = ue::MakeName(id.c_str());
#endif
        if (name.None()) return false;
        (list ? result.shaqi : result.main).push_back(name);
    }
    if (result.main.empty() || result.shaqi.empty()) return false;
    set = std::move(result); return true;
}
void Persist() {
#ifndef PBZ_WEAPON_PRESETS_TEST
    if (!store_loaded) return;
    SettingsStore::PutBool(L"WeaponPresets", L"Enabled", enabled);
    SettingsStore::PutInt(L"WeaponPresets", L"ActiveSet", active_set);
    SettingsStore::Put(L"WeaponPresets", L"Set1", Encode(sets[0]));
    SettingsStore::Put(L"WeaponPresets", L"Set2", Encode(sets[1]));
#endif
}
void LoadStore() {
    store_loaded = true;
#ifndef PBZ_WEAPON_PRESETS_TEST
    Set first, second;
    const bool has_first = Decode(SettingsStore::Get(L"WeaponPresets", L"Set1"), first);
    const bool has_second = Decode(SettingsStore::Get(L"WeaponPresets", L"Set2"), second);
    if (has_first) sets[0] = first;
    if (has_second) sets[1] = second;
    active_set = std::clamp(SettingsStore::GetInt(L"WeaponPresets", L"ActiveSet", 0), 0, 1);
    if (!sets[active_set].known) active_set = 0;
    if ((has_first || has_second) && SettingsStore::GetBool(L"WeaponPresets", L"Enabled", false)) {
        enabled = true; Status(L"已恢复上次保存的两套武器预设与开关；切换前仍核对实际装备");
    }
    if (has_first || has_second) Logf("WEAPON_PRESETS restored saved banks set1=%d set2=%d active=%d enabled=%d", has_first ? 1 : 0, has_second ? 1 : 0, active_set + 1, enabled ? 1 : 0);
#endif
}
void Publish(bool ready, bool busy = false) {
    Persist();
    auto a = View(sets[0]), b = View(sets[1]), current = View(current_equipment);
    AcquireSRWLockExclusive(&snapshot_lock);
    snapshot.enabled = enabled; snapshot.ready = ready; snapshot.busy = busy || rollback_pending;
    snapshot.current_set = active_set; snapshot.has_alternate = sets[1].known;
    snapshot.sets[0] = std::move(a); snapshot.sets[1] = std::move(b);
    snapshot.current = std::move(current);
    snapshot.can_save=ready&&current_equipment.known&&current_equipment.bankable&&!rollback_pending;
    snapshot.current_special_slot=current_equipment.known&&!current_equipment.bankable;
    ReleaseSRWLockExclusive(&snapshot_lock);
}
void Failed() { AcquireSRWLockExclusive(&snapshot_lock); ++snapshot.failures; ReleaseSRWLockExclusive(&snapshot_lock); }
bool ReadCurrent(const Context& c);
struct PendingApply {
    Set goal,submitted;
    Identity pawn,world;
    int target=0;
    bool selecting=false,restoring=false,restore_needed=false,superseded=false,timed_out=false;
    uint64_t active_wait=0;
    ULONGLONG last_tick=0;
    std::wstring error;
} transaction;
bool SameLayout(const Set& a,const Set& b){return a.known&&b.known&&a.main==b.main&&a.shaqi==b.shaqi;}
bool ReadLayout(Object* backpack,Set& layout){
#ifdef PBZ_WEAPON_PRESETS_TEST
    if(test_backend.capture)return test_backend.capture(backpack,layout);
#endif
    Set out;if(!ArrayNames(backpack,"MainWeapons",out.main)||!ArrayNames(backpack,"ShaqiWeapons",out.shaqi))return false;out.known=true;layout=std::move(out);return true;
}
void RetireTransaction(){rollback_pending=false;rollback_backpack=nullptr;rollback_identity={};transaction=PendingApply{};}
bool Paused(const Context& c,bool& value) {
#ifdef PBZ_WEAPON_PRESETS_TEST
    if(test_backend.paused)return test_backend.paused(c,value);
#endif
    Call paused(ue::Default("/Script/Engine.Default__GameplayStatics"),"IsGamePaused");
    if(!paused.Slot("ReturnValue",1)||!paused.Set("WorldContextObject",c.world).Invoke())return false;
    value=paused.Get<bool>("ReturnValue");return true;
}
bool TransactionOwner(const Context& c){return Same(rollback_identity)&&Same(transaction.pawn)&&Same(transaction.world)&&
    c.backpack==rollback_backpack&&c.pawn==transaction.pawn.object&&c.world==transaction.world.object&&ue::NameOf(c.backpack)==rollback_name;}
void AdvanceTransaction(const Context& c,bool& ready) {
    if(!rollback_pending)return;
    const ULONGLONG now=GetTickCount64();bool paused=false;
    const bool pause_known=Paused(c,paused);
    if(transaction.last_tick&&pause_known&&!paused&&!last_runtime_special&&now>=transaction.last_tick)transaction.active_wait+=now-transaction.last_tick;
    transaction.last_tick=now;
    if(!TransactionOwner(c)) {enabled=false;Status(L"装备请求所属主角/世界已变化；保留恢复快照，未对新主角变更装备");return;}
    if(!ConfigHelpersMatch()){DetailedStatus(L"原生入口已变化，未继续变更装备");return;}
    if(!SameInventory(c.backpack,true,rollback_main)||!SameInventory(c.backpack,false,rollback_sha)){
        enabled=false;transaction.superseded=true;transaction.error=diagnostic;
    }
    Set layout;
    if(!ReadLayout(c.backpack,layout)) {DetailedStatus(L"等待原生装备列表稳定");return;}
    if(transaction.submitted.known&&!SameLayout(layout,transaction.submitted))transaction.superseded=true;
    int jobs=0;
    if(!ConfigRequests(c.backpack,jobs)){DetailedStatus(L"等待原生装备请求退休");return;}
    if(transaction.active_wait>=8000&&!transaction.timed_out){transaction.timed_out=true;Failed();Logf("WEAPON_PRESETS unpaused wait budget exhausted; pending_jobs=%d no replay/no immediate rollback",jobs);}
    if(jobs){
        Status(paused?L"世界已暂停，等待原生装备资源回调；需要时返回游戏继续，未擅自解除暂停":
            (transaction.timed_out?L"原生装备加载超时但请求尚未退休；继续只读等待，未回放替换或抢先恢复":L"原生装备资源加载中；请求仅提交一次，等待配置回调退休"));return;
    }
    if(transaction.superseded){enabled=false;RetireTransaction();Status(L"游戏装备菜单已另行更改武器；原生请求已退休，保留用户当前装备，未盲目恢复覆盖");return;}
    if(last_runtime_special){Status(L"当前使用额外特殊武器槽，等待回到普通武器后继续提交/清理；未抢占临时武器");return;}
    if(!ready){
        if(!transaction.timed_out){DetailedStatus(paused?L"世界已暂停，等待读回已提交武器配置":L"等待原生武器配置和已提交选择读回");return;}
        transaction.restore_needed=true;transaction.error=diagnostic;
    }
    if(!transaction.restore_needed&&ready&&!SameLayout(current_equipment,transaction.goal)){
        if(!transaction.timed_out){Status(L"原生回调已退休，等待完整配置列表匹配目标");return;}
        transaction.restore_needed=true;transaction.error=L"原生配置列表未匹配目标";
    }
    if(!transaction.restore_needed&&ready&&!transaction.selecting){
        if(Restricted(c)){DetailedStatus(L"配置已载入，等待主角状态允许提交实际武器选择");return;}
        transaction.selecting=true;
        if(!SelectIndices(c,transaction.goal)){transaction.restore_needed=true;transaction.error=diagnostic;}
        ready=ReadCurrent(c);
    }
    if(!transaction.restore_needed&&ready&&Equal(current_equipment,transaction.goal)){
        const int completed=transaction.target;const bool restored=transaction.restoring;
        RetireTransaction();faulted=false;
        if(restored){enabled=false;Status(L"原生请求已退休，已核对恢复离开前的实际装备和选择");}
        else {active_set=completed;AcquireSRWLockExclusive(&snapshot_lock);++snapshot.swaps;ReleaseSRWLockExclusive(&snapshot_lock);
            Status(active_set?L"已应用第二套；原生配置和实际已提交选择均已核对":L"已应用原始套；原生配置和实际已提交选择均已核对");
            Logf("WEAPON_PRESETS switched set=%d full runtime config identity and committed indices verified; asynchronous jobs retired",active_set+1);}
        return;
    }
    if(!transaction.restore_needed&&transaction.timed_out){transaction.restore_needed=true;transaction.error=L"原生实际武器选择未完成目标提交";}
    if(!transaction.restore_needed){Status(paused?L"世界已暂停，等待实际武器选择提交；返回游戏可继续，暂停未被修改":L"实际武器选择请求已提交一次，等待原生已提交索引读回");return;}
    // An old load callback may still fire after a timeout. Restoration starts
    // only after the per-slot request table retired, and each late callback is
    // additionally guarded by the game's slot-key/generation check.
    if(Restricted(c)){DetailedStatus(L"请求已退休，等待安全状态再恢复原装备");return;}
    if(ready&&Equal(current_equipment,rollback)) {enabled=false;RetireTransaction();Status(L"原生请求已退休，离开前装备仍保持；已停止本次切换");return;}
    if(rollback_attempts>=3){enabled=false;Status(L"原装备恢复仍被游戏拒绝；已停止重试并保留快照，请退出重载存档");return;}
    if(now<next_rollback)return;
    ++rollback_attempts;next_rollback=now+1000;
    transaction.goal=rollback;transaction.restoring=true;transaction.restore_needed=false;transaction.selecting=false;
    transaction.active_wait=0;transaction.timed_out=false;transaction.last_tick=now;
    const bool submitted=SubmitEquip(c.backpack,rollback,true);ReadLayout(c.backpack,transaction.submitted);
    if(!submitted){transaction.restore_needed=true;transaction.error=diagnostic;}
    ready=ReadCurrent(c);Status(L"原生请求已退休，已提交原装备恢复；等待配置和选择读回，未重复回放");
}
void Swap(const Context& c, int requested_set = -1) {
    Set before;
    if (Restricted(c) || !Capture(c.backpack, before)) { DetailedStatus(L"暂不能应用预设"); return; }
    if(!before.bankable){Status(L"当前使用额外特殊武器槽；已读取真实选择，返回普通武器后才能保存或应用预设");return;}
    if (!sets[active_set].known) sets[active_set] = before;
    const int other = requested_set < 0 ? 1 - active_set : requested_set;
    if (requested_set < 0 && !sets[other].known) sets[other] = before;
    // Refresh the set being left every time, including native equipment-menu edits.
    // An explicit same-bank restore must retain its saved target rather than
    // first replacing it with the currently edited native equipment.
    if (other != active_set) sets[active_set] = before;
    std::vector<Step<Name>> plan; std::vector<OwnedInfo> main, shaqi;
    if (!BuildPlan(c.backpack, before, sets[other], plan, main, shaqi)) {
        Failed(); DetailedStatus(L"未修改装备"); return;
    }
    if(Equal(before,sets[other])){
        active_set=other;AcquireSRWLockExclusive(&snapshot_lock);++snapshot.swaps;ReleaseSRWLockExclusive(&snapshot_lock);
        Status(active_set?L"第二套与当前实际装备一致，已切换预设记忆；无需重建武器":L"原始套与当前实际装备一致，已切换预设记忆；无需重建武器");
        Logf("WEAPON_PRESETS switched set=%d already matches verified runtime configs/indices; equip-writes=0",active_set+1);Publish(true);return;
    }
    Publish(true, true);
    rollback_identity = Identify(c.backpack);
    if (!Same(rollback_identity)) { Status(L"原生背包弱身份校验失败，未修改装备"); Publish(false); return; }
    rollback = before; rollback_backpack = c.backpack; rollback_name = ue::NameOf(c.backpack); rollback_pending = true;
    rollback_main = main; rollback_sha = shaqi; rollback_attempts = 0; next_rollback = 0;
    transaction=PendingApply{};transaction.goal=sets[other];transaction.target=other;transaction.pawn=Identify(c.pawn);transaction.world=Identify(c.world);transaction.last_tick=GetTickCount64();
    next_transaction_poll=transaction.last_tick+100;
    if(!Same(transaction.pawn)||!Same(transaction.world)){RetireTransaction();Status(L"当前主角/世界弱身份校验失败，未提交装备请求");return;}
    const bool submitted=SubmitEquip(c.backpack,transaction.goal);ReadLayout(c.backpack,transaction.submitted);
    if(!submitted){Failed();transaction.restore_needed=true;transaction.error=diagnostic;}
    bool ready=ReadCurrent(c);AdvanceTransaction(c,ready);Publish(ready);
}
bool ReadCurrent(const Context& c) {
    Set read;
    if (!Capture(c.backpack,read)) {
        current_equipment.known=false;
        AcquireSRWLockExclusive(&snapshot_lock); snapshot.read_status=std::wstring(L"当前装备读取失败：")+diagnostic; ReleaseSRWLockExclusive(&snapshot_lock);
        if(last_read_diagnostic!=diagnostic) {last_read_diagnostic=diagnostic;Logf("WEAPON_PRESETS read failed reason=%ls",diagnostic.c_str());}
        return false;
    }
    const bool changed=!Equal(current_equipment,read); current_equipment=std::move(read);
    last_read_diagnostic.clear();
    if(current_equipment.bankable&&!sets[0].known&&!sets[1].known) {
        sets[0]=current_equipment; active_set=0;
        Logf("WEAPON_PRESETS first actual read captured original set=1 equip-writes=0");
    }
    AcquireSRWLockExclusive(&snapshot_lock);
    ++snapshot.live_reads; snapshot.last_read_tick=GetTickCount64();
    snapshot.read_status=current_equipment.bankable?L"已读取原生实际装备和已提交选择；全部普通/杀气ID配置身份已核对":
        L"已读取真实额外特殊武器槽；上方仍展示已装备ID，返回普通武器后才能保存或应用预设";
    ReleaseSRWLockExclusive(&snapshot_lock);
    if (changed) {
        char a[160]{},b[160]{},x[160]{},y[160]{};
        ue::NameText(current_equipment.main[0],a,sizeof(a)); if(current_equipment.main.size()>1)ue::NameText(current_equipment.main[1],b,sizeof(b));
        ue::NameText(current_equipment.shaqi[0],x,sizeof(x)); if(current_equipment.shaqi.size()>1)ue::NameText(current_equipment.shaqi[1],y,sizeof(y));
        Logf("WEAPON_PRESETS read current main=[%s,%s] shaqi=[%s,%s] selection=%d/%d actual-readback=1",a,b,x,y,current_equipment.main_index,current_equipment.shaqi_index);
    }
    return true;
}
void SaveCurrent(int target) {
    if(!current_equipment.known||!current_equipment.bankable){Status(L"额外特殊武器槽不属于普通预设；未保存错误选择，请先返回普通武器");return;}
    sets[target]=current_equipment; active_set=target;
    Status(target?L"当前实际装备已保存到第二套；原始套内容保持；未调用装备变更":L"当前实际装备已保存到原始套；第二套内容保持；未调用装备变更");
    Logf("WEAPON_PRESETS saved current to set=%d actual-readback=1 equip-writes=0",target+1);
}
void Tick() {
    if (!store_loaded) { LoadStore(); Publish(false); }
    int enable_request = -1, action = 0;
    bool read_requested=false;
    AcquireSRWLockExclusive(&request_lock); enable_request = pending_enable; action = pending_action; read_requested=pending_read; pending_enable = -1; pending_action = 0; pending_read=false; ReleaseSRWLockExclusive(&request_lock);
    if (enable_request >= 0) {
        enabled = enable_request != 0;
        Status(enabled ? L"预设已开启：第一套记忆当前，第二套首次复制当前；切到第二套后可在原生装备菜单修改" : L"第二套武器预设已关闭；保留当前装备和两套记忆");
        if(rollback_pending){
            if(!enabled){transaction.restore_needed=true;transaction.error=L"用户关闭了第二套武器预设";Status(L"已关闭预设；等待原生请求退休后恢复离开前装备，不再提交目标套的选择");}
            else if(transaction.restore_needed||transaction.restoring){enabled=false;Status(L"原装备清理尚未完成，请完成后再重新开启预设");}
        }
    }
    const ULONGLONG now=GetTickCount64();
    if (!enabled && !read_requested && !action && !rollback_pending && enable_request<0) return;
    if(rollback_pending&&!action&&!read_requested&&enable_request<0&&now<next_transaction_poll)return;
    if(rollback_pending)next_transaction_poll=now+100;
    if (!action && !read_requested && !rollback_pending && enable_request<0 && now<next_read) return;
    next_read=now+250;
    Context c;
    diagnostic.clear(); const bool context_ready = ContextNow(c);
    bool ready=context_ready&&ReadCurrent(c);
    if (!context_ready) {
        current_equipment.known=false;
        AcquireSRWLockExclusive(&snapshot_lock); snapshot.read_status=std::wstring(L"等待读取当前装备：")+diagnostic; ReleaseSRWLockExclusive(&snapshot_lock);
        if(last_read_diagnostic!=diagnostic) {last_read_diagnostic=diagnostic;Logf("WEAPON_PRESETS context not ready reason=%ls",diagnostic.c_str());}
    }
    if (rollback_pending) {
        if(context_ready)AdvanceTransaction(c,ready);
        Publish(ready); return;
    }
    if (!ready) { if (action || enable_request>=0) DetailedStatus(L"操作未执行"); Publish(false); return; }
    if (faulted && (action==1 || action==20 || action==21)) { Status(L"之前的原生装备调用出现异常，请重载游戏后再应用预设；仍可读取和保存当前"); Publish(true); return; }
    if (enabled&&current_equipment.bankable) {
        if(!sets[active_set].known)sets[active_set]=current_equipment;
        if(!sets[1-active_set].known)sets[1-active_set]=current_equipment;
    }
    if (action==2) SaveCurrent(1-active_set);
    else if (action==10 || action==11) SaveCurrent(action-10);
    else if (action==1 || action==20 || action==21) {
        if(enabled) { Swap(c,action==1?-1:action-20); ready=ReadCurrent(c); }
        else Status(L"请先开启第二套预设，再应用武器装备；读取和保存不需要开启");
    }
    if (now >= next_publish || enable_request >= 0 || action || read_requested) { Publish(ready); next_publish = now + 250; }
}

bool DualSense(HANDLE device, USHORT& report_size) {
    HIDD_ATTRIBUTES attributes{}; attributes.Size = sizeof(attributes);
    if (!HidD_GetAttributes(device, &attributes) || attributes.VendorID != 0x054c ||
        (attributes.ProductID != 0x0ce6 && attributes.ProductID != 0x0df2)) return false;
    PHIDP_PREPARSED_DATA preparsed = nullptr; HIDP_CAPS caps{};
    if (!HidD_GetPreparsedData(device, &preparsed)) return false;
    const bool valid = HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS && caps.UsagePage == 1 &&
        (caps.Usage == 4 || caps.Usage == 5) && (caps.InputReportByteLength == 64 || caps.InputReportByteLength == 78);
    if (valid) report_size = caps.InputReportByteLength;
    HidD_FreePreparsedData(preparsed); return valid;
}
HANDLE OpenPad(USHORT& report_size) {
    GUID guid{}; HidD_GetHidGuid(&guid);
    HDEVINFO devices = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    HANDLE result = INVALID_HANDLE_VALUE;
    for (DWORD i = 0; WaitForSingleObject(hid_stop, 0) != WAIT_OBJECT_0; ++i) {
        SP_DEVICE_INTERFACE_DATA interface_data{}; interface_data.cbSize = sizeof(interface_data);
        if (!SetupDiEnumDeviceInterfaces(devices, nullptr, &guid, i, &interface_data)) break;
        DWORD bytes = 0; SetupDiGetDeviceInterfaceDetailW(devices, &interface_data, nullptr, 0, &bytes, nullptr);
        if (bytes < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) || bytes > 65536) continue;
        std::vector<unsigned char> detail_storage(bytes);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detail_storage.data()); detail->cbSize = sizeof(*detail);
        if (!SetupDiGetDeviceInterfaceDetailW(devices, &interface_data, detail, bytes, nullptr, nullptr)) continue;
        // Query first with zero access; do not open unrelated HID data streams.
        HANDLE query = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (query == INVALID_HANDLE_VALUE) continue;
        const bool matches = DualSense(query, report_size); CloseHandle(query);
        if (!matches) continue;
        result = CreateFileW(detail->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (result != INVALID_HANDLE_VALUE) break;
        InterlockedExchange(&hid_error, static_cast<LONG>(GetLastError()));
    }
    SetupDiDestroyDeviceInfoList(devices); return result;
}
DWORD WINAPI HidReader(void*) {
    WeaponPresetsLogic::Swipe swipe;
    HANDLE device = INVALID_HANDLE_VALUE;
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) { InterlockedExchange(&hid_error, static_cast<LONG>(GetLastError())); return 0; }
    unsigned char report[128]{};
    USHORT report_size = 0;
    while (WaitForSingleObject(hid_stop, 0) != WAIT_OBJECT_0) {
        if (device == INVALID_HANDLE_VALUE) {
            device = OpenPad(report_size); swipe.Reset();
            if (device == INVALID_HANDLE_VALUE) { WaitForSingleObject(hid_stop, 2000); continue; }
        }
        OVERLAPPED io{}; io.hEvent = event; ResetEvent(event);
        DWORD bytes = 0; bool complete = ReadFile(device, report, report_size, &bytes, &io) != FALSE;
        if (!complete && GetLastError() == ERROR_IO_PENDING) {
            HANDLE events[] = {hid_stop, event};
            const DWORD wait = WaitForMultipleObjects(2, events, FALSE, 1000);
            if (wait != WAIT_OBJECT_0 + 1) {
                CancelIoEx(device, &io); GetOverlappedResult(device, &io, &bytes, TRUE);
                if (wait == WAIT_OBJECT_0) break;
                swipe.Reset(); InterlockedExchange(&hid_ready, 0); continue;
            }
            complete = GetOverlappedResult(device, &io, &bytes, FALSE) != FALSE;
        }
        if (!complete) {
            InterlockedExchange(&hid_error, static_cast<LONG>(GetLastError())); InterlockedExchange(&hid_ready, 0);
            CloseHandle(device); device = INVALID_HANDLE_VALUE; swipe.Reset(); continue;
        }
        if (bytes > 0) UiHotkeys::NoteDualSenseTouch(report, static_cast<int>(bytes));
        WeaponPresetsLogic::Touch touch;
        if (!WeaponPresetsLogic::DecodeTouch(report, bytes, touch)) { swipe.Reset(); continue; }
        InterlockedExchange(&hid_ready, 1); InterlockedIncrement(&hid_reports);
        if (swipe.Feed(touch, GetTickCount64(), InterlockedCompareExchange(&hid_allowed, 0, 0) != 0)) InterlockedIncrement(&hid_swipes);
    }
    if (device != INVALID_HANDLE_VALUE) CloseHandle(device);
    CloseHandle(event); InterlockedExchange(&hid_ready, 0); return 0;
}
}

void Initialize(Logger log, InputFocus focus) { logger = log; input_focus = focus; Logf("WEAPON_PRESETS initialized default-off; native equipped IDs only, independent read-only DualSense touch stream"); }
bool RequestEnable(bool value) {
    AcquireSRWLockExclusive(&request_lock); pending_enable = value ? 1 : 0; if (!value) pending_action = 0; ReleaseSRWLockExclusive(&request_lock); return true;
}
bool RequestSwap() {
    AcquireSRWLockExclusive(&request_lock); const bool accepted = pending_action == 0; if (accepted) pending_action = 1; ReleaseSRWLockExclusive(&request_lock); return accepted;
}
bool RequestCopyCurrentToOther() {
    AcquireSRWLockExclusive(&request_lock); const bool accepted = pending_action == 0; if (accepted) pending_action = 2; ReleaseSRWLockExclusive(&request_lock); return accepted;
}
bool RequestReadCurrent() {
    AcquireSRWLockExclusive(&request_lock); pending_read=true; ReleaseSRWLockExclusive(&request_lock); return true;
}
bool RequestSaveToSet(int32_t set_index) {
    if(set_index<0||set_index>1)return false;
    AcquireSRWLockExclusive(&request_lock); const bool accepted=pending_action==0; if(accepted)pending_action=10+set_index; ReleaseSRWLockExclusive(&request_lock); return accepted;
}
bool RequestSwitchToSet(int32_t set_index) {
    if(set_index<0||set_index>1)return false;
    AcquireSRWLockExclusive(&request_lock); const bool accepted=pending_action==0; if(accepted)pending_action=20+set_index; ReleaseSRWLockExclusive(&request_lock); return accepted;
}
Snapshot GetSnapshot() { AcquireSRWLockShared(&snapshot_lock); Snapshot result = snapshot; ReleaseSRWLockShared(&snapshot_lock); return result; }
void WorkerTick() {
    const Snapshot state = GetSnapshot();
    const bool allowed = state.enabled && state.ready && state.can_save && !state.busy && input_focus && input_focus();
    InterlockedExchange(&hid_allowed, allowed ? 1 : 0);
    if (state.enabled && !hid_thread) {
        UiHotkeys::YieldSony(true);
        hid_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (hid_stop) hid_thread = CreateThread(nullptr, 0, HidReader, nullptr, 0, nullptr);
        if (!hid_thread && hid_stop) { CloseHandle(hid_stop); hid_stop = nullptr; }
    } else if (!state.enabled && hid_thread) SetEvent(hid_stop);
    if (hid_thread && WaitForSingleObject(hid_thread, 0) == WAIT_OBJECT_0) {
        CloseHandle(hid_thread); CloseHandle(hid_stop); hid_thread = nullptr; hid_stop = nullptr;
    }
    UiHotkeys::YieldSony(hid_thread != nullptr);
    const LONG edges = InterlockedExchange(&hid_swipes, 0);
    // The weapon-swap hotkey is the only gesture that calls RequestSwap.
    AcquireSRWLockExclusive(&snapshot_lock);
    snapshot.touchpad_ready = InterlockedCompareExchange(&hid_ready, 0, 0) != 0;
    if (edges > 0 && allowed) snapshot.swipes += static_cast<uint32_t>(edges);
    if (!state.enabled) snapshot.touchpad_status = L"触控板监听未开启";
    else if (snapshot.touchpad_ready) snapshot.touchpad_status = L"DualSense 触控板已连接；单指左滑或右滑切换，松开后可再次切换";
    else if (InterlockedCompareExchange(&hid_error, 0, 0) == ERROR_ACCESS_DENIED) snapshot.touchpad_status = L"DualSense HID 被独占；请关闭 HidHide/手柄独占后再试，仍可使用面板切换";
    else snapshot.touchpad_status = L"等待 DualSense 完整触控报告（USB/蓝牙）；XInput 不提供滑动数据";
    ReleaseSRWLockExclusive(&snapshot_lock);
}
void OnGameThreadGuarded() {
    DWORD exception = 0;
    __try { Tick(); } __except (EXCEPTION_EXECUTE_HANDLER) { exception = GetExceptionCode(); }
    if (exception) {
        enabled = false; faulted = true; Failed(); Publish(false);
        Status(rollback_pending ? L"装备切换出现异常；已停用并保留原装备恢复快照" : L"装备接口出现异常，第二套预设已停用");
        Logf("WEAPON_PRESETS guarded exception 0x%08lx; rollback_pending=%d", exception, rollback_pending ? 1 : 0);
    }
}
bool BeforeTravel() { if (rollback_pending) return false; cached_backpack = {}; current_equipment.known=false; Publish(false); return true; }
bool CurrentMainWeaponId(std::string& id) {
    Context c; Name current;
    if (!ContextNow(c) || !Read(c.backpack, "CurrentMainWeaponID", 0x2000, current)) return false;
    char text[128]{}; ue::NameText(current, text, sizeof(text)); id = text; return !id.empty();
}
void Shutdown() {
    InterlockedExchange(&hid_allowed, 0);
    if (hid_stop) SetEvent(hid_stop);
    if (hid_thread && WaitForSingleObject(hid_thread, 2000) == WAIT_OBJECT_0) {
        CloseHandle(hid_thread); CloseHandle(hid_stop); hid_thread = nullptr; hid_stop = nullptr;
    }
}
bool HandleCommand(const char* verb, const char* arguments) {
    if (!verb || std::strcmp(verb, "weapons")) return false;
    std::istringstream stream(arguments ? arguments : ""); std::string action; stream >> action;
    if (action == "on") return RequestEnable(true);
    if (action == "off") return RequestEnable(false);
    if (action == "swap") return RequestSwap();
    if (action == "copy") return RequestCopyCurrentToOther();
    if (action == "read") return RequestReadCurrent();
    int set_index=0;
    if(action=="save" && stream>>set_index)return RequestSaveToSet(set_index-1);
    if(action=="apply" && stream>>set_index)return RequestSwitchToSet(set_index-1);
    if (action == "status" || action.empty()) { const auto state = GetSnapshot(); Logf("WEAPON_PRESETS enabled=%d ready=%d set=%d swaps=%u failures=%u touch=%d current_known=%d live_reads=%u read_status=%ls status=%ls",state.enabled?1:0,state.ready?1:0,state.current_set+1,state.swaps,state.failures,state.touchpad_ready?1:0,state.current.known?1:0,state.live_reads,state.read_status.c_str(),state.status.c_str()); return true; }
    return false;
}
}
