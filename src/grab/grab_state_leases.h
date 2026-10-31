#pragma once
#include "ue_runtime.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <tuple>
#include <utility>
#include <initializer_list>
#include <vector>

// Game-thread only. Capture precedes the native pair. VerifyOrRestore is called
// ONLY after every owned native ability/task (including reversed phases) ended.
// This leases runtime component state; it never edits equipment or save data.
namespace GrabStateLeases {
using ue::Object; using ue::Name;
struct Policy {
    bool collision_enabled=false, main_weapon_index=false, main_armed=false;
    int required_main_index=-1;
    uint32_t allowed_collision_mask=0, allowed_index_mask=0;
    bool allowed_armed=false;
    uint8_t allowed_armed_mask=0; // 0: legacy bool; bit0:false, bit1:true.
    bool Any() const { return collision_enabled||main_weapon_index||main_armed||required_main_index>=0; }
    bool Weapons() const { return main_weapon_index||main_armed||required_main_index>=0; }
};
struct Weak { int32_t index=-1,serial=0; };
struct Identity { Object* object=nullptr; Name name; Weak weak; };
struct MainState {
    void* storage=nullptr; int32_t count=0,capacity=0,committed=-1,pending=-1;
    Name tag; bool armed=false,transition=false;
    std::vector<Object*> configs; std::vector<uintptr_t> interfaces;
};
struct Side {
    Policy policy; Identity actor,capsule,weapons;
    uint8_t collision=0; MainState main;
    std::vector<Identity> configs;
    bool collision_pending=false,index_pending=false,armed_pending=false;
    bool own_index_armed_valid=false,own_index_armed=false,armed_restore=false;
    bool HasPending() const { return collision_pending||index_pending||armed_pending; }
};
struct Ledger {
    Side source,victim; uint32_t preserved=0;
    bool HasPending() const { return source.HasPending()||victim.HasPending(); }
};
enum Preserve : uint32_t { ActorRetired=1, DeadActor=2, ComponentChanged=4, LayoutChanged=8, ForeignValue=16, AmbiguousArmed=32 };
inline bool ArmedAllowed(const Policy& p,bool value) { return p.allowed_armed_mask?(p.allowed_armed_mask&(value?2:1))!=0:value==p.allowed_armed; }
inline bool MaskHas(uint32_t mask,int value) { return value>=0&&value<32&&(mask&(uint32_t{1}<<value)); }
inline void Retire(Side& side) { side.collision_pending=side.index_pending=side.armed_pending=false; }
inline bool LayoutEqual(const MainState& a,const MainState& b) {
    return a.storage==b.storage&&a.count==b.count&&a.capacity==b.capacity&&a.tag==b.tag&&a.configs==b.configs&&a.interfaces==b.interfaces;
}
inline bool Reject(std::wstring& reason,const wchar_t* value) { reason=value; return false; }

// IO is intentionally typed: the fixture instantiates these SAME production
// transactions, while native reads/setters remain a narrow I/O boundary.
template<class IO> bool CaptureSide(IO& io,Object* actor,const Policy& policy,Side& side,std::wstring& reason) {
    if(!policy.Any())return true;
    if(policy.allowed_armed_mask>3||policy.required_main_index < -1 || policy.required_main_index>=32 ||
       (policy.collision_enabled&&(!policy.allowed_collision_mask||(policy.allowed_collision_mask&~uint32_t{0x3f})))||
       (policy.main_weapon_index&&!policy.allowed_index_mask))return Reject(reason,L"原生状态恢复策略无效");
    side.policy=policy; side.actor=io.Identify(actor);
    if(!io.Same(side.actor))return Reject(reason,L"原生状态恢复：角色代际身份不可用");
    bool dead=false;if(!io.IsDead(actor,dead))return Reject(reason,L"无法读取角色原生生命状态");
    if(dead)return Reject(reason,L"原生状态记录要求角色仍然存活");
    if(policy.collision_enabled) {
        Object* capsule=nullptr;
        if(!io.Capsule(actor,capsule)||(side.capsule=io.Identify(capsule),!io.Same(side.capsule))||
           !io.Collision(capsule,side.collision)||side.collision>5)return Reject(reason,L"胶囊碰撞模式预检失败");
        side.collision_pending=true;
    }
    if(policy.Weapons()) {
        Object* weapons=nullptr;
        if(!io.Weapons(actor,weapons)||(side.weapons=io.Identify(weapons),!io.Same(side.weapons))||!io.Main(weapons,side.main))
            return Reject(reason,L"主武器运行时槽位预检失败");
        if(side.main.count<1||side.main.count>32||side.main.configs.size()!=static_cast<size_t>(side.main.count)||
           side.main.interfaces.size()!=side.main.configs.size()||side.main.committed<0||side.main.committed>=side.main.count||
           side.main.pending!=side.main.committed||side.main.transition)
            return Reject(reason,L"主武器槽位缺失或仍有待提交切换");
        if(policy.required_main_index>=side.main.count)return Reject(reason,L"招式要求的主武器索引不存在");
        if(policy.main_weapon_index && (policy.allowed_index_mask & ~(side.main.count==32?~uint32_t{0}:((uint32_t{1}<<side.main.count)-1))))
            return Reject(reason,L"招式允许的主武器索引超出当前槽位布局");
        for(Object* config:side.main.configs) {
            Identity id=io.Identify(config);if(!io.Same(id))return Reject(reason,L"主武器配置对象代际身份不可用");
            side.configs.push_back(id);
        }
        side.index_pending=policy.main_weapon_index;side.armed_pending=policy.main_armed;side.armed_restore=side.main.armed;
    }
    return true;
}
template<class IO> bool CaptureWith(IO& io,Object* source,Object* victim,const Policy& sp,const Policy& vp,Ledger& ledger,std::wstring& reason) {
    reason.clear();
    if(ledger.HasPending())return Reject(reason,L"上一组原生状态恢复记录尚未完成");
    if(!sp.Any()&&!vp.Any()){ledger={};return true;} // Strict zero-I/O fast path.
    Ledger next;
    if(!CaptureSide(io,source,sp,next.source,reason)||!CaptureSide(io,victim,vp,next.victim,reason))return false;
    ledger=std::move(next);return true;
}
// A native setter may synchronously run callbacks which replace or retire
// actor/components/configs. Recheck before any further native reads or writes.
template<class IO> int RecheckWeapons(IO& io,Side& side,uint32_t& preserved,std::wstring& reason) {
    if(!io.Same(side.actor)){Retire(side);preserved|=ActorRetired;return 0;}
    if(!io.Same(side.weapons)){side.index_pending=side.armed_pending=false;preserved|=ComponentChanged;return 0;}
    Object* current=nullptr;if(!io.Weapons(side.actor.object,current)){reason=L"等待主武器组件身份读回";return -1;}
    if(current!=side.weapons.object){side.index_pending=side.armed_pending=false;preserved|=ComponentChanged;return 0;}
    for(const Identity& config:side.configs)if(!io.Same(config)){side.index_pending=side.armed_pending=false;preserved|=LayoutChanged;return 0;}
    return 1;
}
inline void ObserveIndexArmed(Side& side,bool before,bool after) {
    if(after==before)return;
    side.own_index_armed_valid=true;side.own_index_armed=after;
    // Only an immediate read on this GT attributes a side effect to this
    // setter. Never infer ownership from a later frame after a failed read.
    if(!side.armed_pending){side.armed_pending=true;side.armed_restore=before;}
}
template<class IO> bool RestoreSide(IO& io,Side& side,uint32_t& preserved,std::wstring& reason) {
    if(!side.HasPending())return true;
    if(!io.Same(side.actor)){Retire(side);preserved|=ActorRetired;return true;}
    bool dead=false;if(!io.IsDead(side.actor.object,dead))return Reject(reason,L"等待角色原生生命状态读回");
    if(dead){Retire(side);preserved|=DeadActor;return true;} // Never resurrect corpse collision/weapons.
    bool complete=true;
    if(side.collision_pending) {
        Object* capsule=nullptr;
        if(!io.Capsule(side.actor.object,capsule)){complete=false;reason=L"等待胶囊组件身份读回";}
        else if(capsule!=side.capsule.object||!io.Same(side.capsule)){side.collision_pending=false;preserved|=ComponentChanged;}
        else {
            uint8_t value=0;
            if(!io.Collision(capsule,value)){complete=false;reason=L"等待胶囊碰撞模式读回";}
            else if(value==side.collision)side.collision_pending=false;
            else if(!MaskHas(side.policy.allowed_collision_mask,value)){side.collision_pending=false;preserved|=ForeignValue;}
            else {
                io.SetCollision(capsule,side.collision); // A reported failure may have written: always read back.
                if(!io.Same(side.actor)){Retire(side);preserved|=ActorRetired;return complete;}
                if(!io.Same(side.capsule)){side.collision_pending=false;preserved|=ComponentChanged;}
                else if(!io.Capsule(side.actor.object,capsule)){complete=false;reason=L"等待胶囊组件身份读回";}
                else if(capsule!=side.capsule.object){side.collision_pending=false;preserved|=ComponentChanged;}
                else if(io.Collision(capsule,value)&&value==side.collision)side.collision_pending=false;
                else {complete=false;reason=L"等待原生胶囊碰撞模式恢复读回";}
            }
        }
    }
    if(side.index_pending||side.armed_pending) {
        if(!io.Same(side.actor)){Retire(side);preserved|=ActorRetired;return complete;}
        if(!io.IsDead(side.actor.object,dead))return Reject(reason,L"等待角色原生生命状态读回");
        if(dead){Retire(side);preserved|=DeadActor;return complete;}
        Object* weapons=nullptr;
        if(!io.Weapons(side.actor.object,weapons))return Reject(reason,L"等待主武器组件身份读回");
        if(weapons!=side.weapons.object||!io.Same(side.weapons)) {
            side.index_pending=side.armed_pending=false;preserved|=ComponentChanged;return complete;
        }
        MainState now;if(!io.Main(weapons,now))return Reject(reason,L"等待主武器运行时状态读回");
        bool same=LayoutEqual(side.main,now);
        for(const Identity& config:side.configs)if(!io.Same(config)){same=false;break;}
        if(!same){side.index_pending=side.armed_pending=false;preserved|=LayoutChanged;return complete;}
        // Never start a competing native change while an authored/native queue
        // is still active. Foreign pending indices are preserved, not awaited.
        if(now.transition||now.committed!=now.pending) {
            const bool pending_owned=now.pending==side.main.committed||MaskHas(side.policy.allowed_index_mask,now.pending);
            if(!pending_owned){side.index_pending=side.armed_pending=false;preserved|=ForeignValue;return complete;}
            return Reject(reason,L"等待原生主武器切换队列结束");
        }
        if(!side.index_pending&&side.armed_pending&&now.committed!=side.main.committed) {
            side.armed_pending=false;preserved|=ForeignValue;return complete;
        }
        if(side.index_pending) {
            if(now.committed==side.main.committed)side.index_pending=false;
            else if(!MaskHas(side.policy.allowed_index_mask,now.committed)){side.index_pending=false;side.armed_pending=false;preserved|=ForeignValue;}
            else {
                const bool armed_before=now.armed;
                io.SetIndex(weapons,side.main.tag,side.main.committed);
                const int owned=RecheckWeapons(io,side,preserved,reason);if(owned<=0)return owned==0?complete:false;
                bool armed_after=false;const bool armed_known=io.Armed(weapons,side.main.tag,armed_after);
                if(armed_known)ObserveIndexArmed(side,armed_before,armed_after);
                MainState after;
                if(!io.Main(weapons,after)) {
                    if(!armed_known&&!side.policy.main_armed)preserved|=AmbiguousArmed;
                    return Reject(reason,L"等待原生主武器索引恢复读回");
                }
                if(!LayoutEqual(side.main,after)){side.index_pending=side.armed_pending=false;preserved|=LayoutChanged;return complete;}
                if(!armed_known)ObserveIndexArmed(side,armed_before,after.armed);
                if(after.committed!=side.main.committed||after.pending!=after.committed||after.transition)
                    return Reject(reason,L"等待原生主武器索引恢复提交");
                side.index_pending=false;now=std::move(after);
            }
        }
        // Index restoration may itself change armed state; restore armed last.
        if(side.armed_pending) {
            if(now.armed==side.armed_restore)side.armed_pending=false;
            else if(!ArmedAllowed(side.policy,now.armed)&&!(side.own_index_armed_valid&&now.armed==side.own_index_armed)){side.armed_pending=false;preserved|=ForeignValue;}
            else {
                if(!io.IsDead(side.actor.object,dead))return Reject(reason,L"等待角色原生生命状态读回");
                if(dead){Retire(side);preserved|=DeadActor;return complete;}
                io.SetArmed(weapons,side.main.tag,side.armed_restore);
                const int owned=RecheckWeapons(io,side,preserved,reason);if(owned<=0)return owned==0?complete:false;
                MainState after;
                if(!io.Main(weapons,after))return Reject(reason,L"等待原生主武器拔刀状态恢复读回");
                if(!LayoutEqual(side.main,after)){side.armed_pending=false;preserved|=LayoutChanged;return complete;}
                if(after.armed!=side.armed_restore||after.transition||after.pending!=after.committed)
                    return Reject(reason,L"等待原生主武器拔刀状态恢复提交");
                side.armed_pending=false;
            }
        }
    }
    return complete&&!side.HasPending();
}
template<class IO> bool VerifyOrRestoreWith(IO& io,Ledger& ledger,std::wstring& reason,bool native_ended=true) {
    reason.clear();if(!ledger.HasPending())return true; // No engine calls for policy-free moves.
    if(!native_ended)return Reject(reason,L"原生成对能力或任务仍未结束");
    const bool a=RestoreSide(io,ledger.source,ledger.preserved,reason);
    const bool b=RestoreSide(io,ledger.victim,ledger.preserved,reason);
    if(!a||!b||ledger.HasPending())return false;
    if(ledger.preserved)reason=L"原生状态恢复记录已退休；外部改动、未确认的武器副作用、替换对象或死亡角色的状态已保留";
    return true;
}

namespace Detail {
inline bool Readable(const void* p,size_t bytes) {
    if(!p)return false;MEMORY_BASIC_INFORMATION page{};
    if(!VirtualQuery(p,&page,sizeof(page))||page.State!=MEM_COMMIT||(page.Protect&(PAGE_NOACCESS|PAGE_GUARD)))return false;
    const auto at=reinterpret_cast<uintptr_t>(p),start=reinterpret_cast<uintptr_t>(page.BaseAddress);
    return at>=start&&bytes<=page.RegionSize&&at-start<=page.RegionSize-bytes;
}
inline Object* Field(Object* type,const char* name) {
    for(int depth=0;type&&depth<64;++depth,type=ue::At<Object*>(type,0x40)) {
        int n=0;for(Object* p=ue::At<Object*>(type,0x50);p&&++n<1024;p=ue::At<Object*>(p,0x10))
            if(ue::NameIs(ue::At<Name>(p,0x20),name))return p;
    }return nullptr;
}
inline uint64_t Kind(Object* p){return p?ue::At<uint64_t>(ue::At<Object*>(p,8),0x18):0;}
inline bool Shape(Object* p,uint64_t kind,int size,int offset=-1) {
    return p&&Kind(p)==kind&&ue::At<int32_t>(p,0x34)==size&&(offset<0||ue::At<int32_t>(p,0x44)==offset);
}
inline const Object* Item(int32_t index) {
    auto* gu=reinterpret_cast<Object*>(ue::Base()+ue::layout::kObjectArray);
    if(index<0||!Readable(gu,40)||index>=ue::At<int32_t>(gu,32))return nullptr;
    Object** chunks=ue::At<Object**>(gu,24);const uint32_t c=static_cast<uint32_t>(index)>>16;
    if(!Readable(chunks,(static_cast<size_t>(c)+1)*8)||!chunks[c])return nullptr;
    Object* item=chunks[c]+(static_cast<uint32_t>(index)&65535)*24ull;return Readable(item,24)?item:nullptr;
}
inline bool Same(const Identity& id) {
    const Object* item=id.weak.serial>0?Item(id.weak.index):nullptr;
    return item&&ue::At<int32_t>(const_cast<Object*>(item),16)==id.weak.serial&&
        ue::At<Object*>(const_cast<Object*>(item),8)==id.object&&ue::Alive(id.object)&&ue::NameOf(id.object)==id.name;
}
inline Identity Identify(Object* object) {
    Identity id;if(!ue::Alive(object))return id;
    static constexpr unsigned char signature[]={0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0x12,0x48,0x8b,0xd9,0x48,0x85,0xd2,0x74};
    const uintptr_t fn=ue::Base()+0x166f2a0;
    if(!ue::Base()||!Readable(reinterpret_cast<void*>(fn),sizeof(signature))||std::memcmp(reinterpret_cast<void*>(fn),signature,sizeof(signature)))return id;
    reinterpret_cast<void(__fastcall*)(Weak*,Object* const*)>(fn)(&id.weak,&object);
    id.object=object;id.name=ue::NameOf(object);return Same(id)?id:Identity{};
}
struct Metadata {
    uintptr_t base=0;Object* weapons=nullptr,*slot=nullptr,*single=nullptr,*tag_type=nullptr,*dead=nullptr;
    Object* primitive=nullptr,*attribute=nullptr;Object* runtime=nullptr;int offset=-1,stride=0,value_offset=0;Name main;
};
inline Metadata metadata;
inline bool Function(Object* type,const char* name,int bytes,std::initializer_list<std::tuple<const char*,uint64_t,int,int>> fields) {
    Object* f=ue::FindFunction(type,name);if(!f||ue::At<uint16_t>(f,ue::layout::kFuncParmsSize)!=bytes)return false;
    for(const auto& field:fields)if(!Shape(Field(f,std::get<0>(field)),std::get<1>(field),std::get<2>(field),std::get<3>(field)))return false;
    return true;
}
inline bool Prepare(std::wstring& reason) {
    if(metadata.base==ue::Base()&&metadata.base&&ue::Alive(metadata.weapons)&&!metadata.main.None())return true;
    Metadata m;m.base=ue::Base();
    m.weapons=ue::FindObject("/Script/SGameCombatSystem.CharacterWeaponsComponent");
    m.slot=ue::FindObject("/Script/SGameCombatSystem.RuntimeWeaponSlotData");m.single=ue::FindObject("/Script/SGameCombatSystem.RuntimeWeaponSingleData");
    m.attribute=ue::FindObject("/Script/GameplayAbilities.GameplayAttributeData");
    Object* holder=ue::FindObject("/Script/SGameCombatSystem.CharacterWeaponsHolder");
    Object* system=ue::Default("/Script/Engine.Default__KismetSystemLibrary");
    m.tag_type=ue::FindObject("/Script/GameplayTags.GameplayTag");m.dead=ue::Default("/Script/SGameAI.Default__SGameAIStateUtils");
    Object* primitive=ue::FindObject("/Script/Engine.PrimitiveComponent");m.primitive=primitive;
    if(!m.base||!m.weapons||!m.slot||!m.single||!m.tag_type||!m.dead||!primitive||!m.attribute||!holder||!system||!ue::FindFunction(ue::ClassOf(system),"DoesImplementInterface")||ue::StructSize(m.attribute)!=16||ue::Offset(m.attribute,"CurrentValue")!=12||ue::StructSize(m.slot)!=32||ue::StructSize(m.single)!=288||ue::StructSize(m.tag_type)!=8)
        return Reject(reason,L"原生状态恢复所需的反射元数据不可用");
    m.runtime=Field(m.weapons,"RuntimeWeaponData");
    Object* weapons=Field(m.slot,"Weapons"),*config=Field(m.single,"WeaponConfig");
    if(!Shape(m.runtime,0x400000000000ull,80)||!Shape(weapons,0x200000,16,0)||!Shape(config,0x1000,16,0))return Reject(reason,L"原生主武器运行时布局不匹配");
    Object* key=ue::At<Object*>(m.runtime,0x70),*value=ue::At<Object*>(m.runtime,0x78),*inner=ue::At<Object*>(weapons,0x78);
    if(!Shape(key,0x100000,8)||ue::At<Object*>(key,0x70)!=m.tag_type||!Shape(value,0x100000,32)||ue::At<Object*>(value,0x70)!=m.slot||
       !Shape(inner,0x100000,288)||ue::At<Object*>(inner,0x70)!=m.single)return Reject(reason,L"原生主武器槽位或配置反射类型不匹配");
    m.offset=ue::At<int32_t>(m.runtime,0x44);m.stride=ue::At<int32_t>(m.runtime,0x8c);m.value_offset=ue::At<int32_t>(m.runtime,0x80);
    if(m.offset<0||m.stride!=48||m.value_offset!=8)return Reject(reason,L"原生主武器稀疏表布局不匹配");
    constexpr uint64_t st=0x100000,bo=0x20000,in=0x80,by=0x1;
    // IsDead's UFunction::ParmsSize at +0x106 is 9 (Pawn@0/8, bool@8/1).
    // properties_size at +0xA8 is the aligned 16 and must not be used here.
    if(!Function(m.weapons,"GetCurrentWeaponIndex",16,{{"SlotTag",st,8,0},{"WithPending",bo,1,8},{"ReturnValue",in,4,12}}))return Reject(reason,L"GetCurrentWeaponIndex 参数布局不匹配");
    if(!Function(m.weapons,"SetCurrentWeapon",13,{{"SlotTag",st,8,0},{"Index",in,4,8},{"ReturnValue",bo,1,12}}))return Reject(reason,L"SetCurrentWeapon 参数布局不匹配");
    if(!Function(m.weapons,"IsArmed",9,{{"SlotTag",st,8,0},{"ReturnValue",bo,1,8}}))return Reject(reason,L"IsArmed 参数布局不匹配");
    if(!Function(m.weapons,"SetArmed",9,{{"SlotTag",st,8,0},{"Armed",bo,1,8}}))return Reject(reason,L"SetArmed 参数布局不匹配");
    if(!Function(m.weapons,"IsInWeaponTransition",1,{{"ReturnValue",bo,1,0}}))return Reject(reason,L"IsInWeaponTransition 参数布局不匹配");
    if(!Function(primitive,"GetCollisionEnabled",1,{{"ReturnValue",by,1,0}}))return Reject(reason,L"GetCollisionEnabled 参数布局不匹配");
    if(!Function(primitive,"SetCollisionEnabled",1,{{"NewType",by,1,0}}))return Reject(reason,L"SetCollisionEnabled 参数布局不匹配");
    if(!Function(ue::ClassOf(m.dead),"IsDead",9,{{"Pawn",0x10000,8,0},{"ReturnValue",bo,1,8}}))return Reject(reason,L"IsDead 参数布局不匹配");
    if(!Function(holder,"GetCharacterWeaponsComponent",8,{{"ReturnValue",0x10000,8,0}}))return Reject(reason,L"GetCharacterWeaponsComponent 参数布局不匹配");
    m.main=ue::MakeName(L"Weapon.Slot.Main");if(m.main.None())return Reject(reason,L"原生主武器槽位标签不可用");
    metadata=m;return true;
}
struct NativeIO {
    Identity Identify(Object* p){return Detail::Identify(p);}bool Same(const Identity& id){return Detail::Same(id);}
    bool IsDead(Object* actor,bool& value) {
        ue::Call c(metadata.dead,"IsDead");if(!c.Slot("ReturnValue",1)||!c.Set("Pawn",actor).Invoke())return false;
        value=c.Get<bool>("ReturnValue");if(value)return true;
        // The native death state can lag the lethal Health write. Match the
        // existing combat preflight: never restore collision/arms at Health 0.
        Object* set_field=Field(ue::ClassOf(actor),"AttributeSetBase");
        if(!Shape(set_field,0x10000,8))return false;const int set_offset=ue::At<int32_t>(set_field,0x44);
        if(set_offset<0||!Readable(actor+set_offset,8))return false;Object* set=ue::At<Object*>(actor,set_offset);
        if(!ue::Alive(set))return false;Object* health=Field(ue::ClassOf(set),"Health");
        if(!Shape(health,0x100000,16)||ue::At<Object*>(health,0x70)!=metadata.attribute)return false;
        const int offset=ue::At<int32_t>(health,0x44);if(offset<0||!Readable(set+offset,16))return false;
        const float hp=ue::At<float>(set,offset+12);if(!std::isfinite(hp))return false;value=hp<=0;return true;
    }
    bool Capsule(Object* actor,Object*& capsule) {
        if(!ue::Alive(actor))return false;Object* p=Field(ue::ClassOf(actor),"CapsuleComponent");
        if(!Shape(p,0x10000,8))return false;const int o=ue::At<int32_t>(p,0x44);if(o<0||!Readable(actor+o,8))return false;
        capsule=ue::At<Object*>(actor,o);return true;
    }
    bool Weapons(Object* actor,Object*& weapons) {
        ue::Call c(actor,"GetCharacterWeaponsComponent","/Script/SGameCombatSystem.CharacterWeaponsHolder");
        if(!c.Slot("ReturnValue",8)||!c.Invoke())return false;weapons=c.Get<Object*>("ReturnValue");return true;
    }
    bool Collision(Object* capsule,uint8_t& value) {
        if(!ue::Alive(capsule)||!ue::IsA(capsule,metadata.primitive))return false;
        ue::Call c(capsule,"GetCollisionEnabled");if(!c.Slot("ReturnValue",1)||!c.Invoke())return false;value=c.Get<uint8_t>("ReturnValue");return value<=5;
    }
    bool SetCollision(Object* capsule,uint8_t value){if(!ue::Alive(capsule)||!ue::IsA(capsule,metadata.primitive))return false;return ue::Call(capsule,"SetCollisionEnabled").Set("NewType",value).Invoke();}
    bool Main(Object* component,MainState& output) {
        if(!ue::Alive(component)||!ue::IsA(component,metadata.weapons)||Field(ue::ClassOf(component),"RuntimeWeaponData")!=metadata.runtime||
           !Readable(component+metadata.offset,80))return false;
        auto* map=component+metadata.offset;const int32_t count=ue::At<int32_t>(map,8),capacity=ue::At<int32_t>(map,12),bits=ue::At<int32_t>(map,40),max_bits=ue::At<int32_t>(map,44),free=ue::At<int32_t>(map,52);
        if(count<0||count>64||capacity<count||capacity>1024||bits!=count||max_bits<bits||free<0||free>count)return false;
        auto* rows=ue::At<unsigned char*>(map,0);auto* allocation=ue::At<uint32_t*>(map,32);if(!allocation)allocation=reinterpret_cast<uint32_t*>(map+16);
        if(!count){output={};output.tag=metadata.main;return true;} // A stable removed Main slot is a foreign layout, not a retry.
        if(!Readable(rows,static_cast<size_t>(count)*metadata.stride)||!Readable(allocation,(static_cast<size_t>(bits)+31)/32*4))return false;
        unsigned char* found=nullptr;
        for(int32_t i=0;i<count;++i)if(allocation[i/32]&(uint32_t{1}<<(i%32))) {
            auto* row=rows+static_cast<size_t>(i)*metadata.stride;if(ue::At<Name>(row,0)==metadata.main){if(found)return false;found=row+metadata.value_offset;}
        }
        if(!found){output={};output.tag=metadata.main;return true;}
        const ue::Array array=ue::At<ue::Array>(found,0);
        if(array.num<0||array.num>1024||array.max<array.num||array.max>1024)return false;
        MainState now;now.tag=metadata.main;now.storage=array.data;now.count=array.num;now.capacity=array.max;
        if(!array.num||array.num>32){output=std::move(now);return true;} // Capture refuses; existing lease retires without writes.
        if(!Readable(array.data,static_cast<size_t>(array.num)*288))return false;
        // Read existing engine-owned storage, never invoke GetCurrentWeapon or
        // GetSlotWeapon: their 288-byte outputs contain owning TArray copies.
        for(int32_t i=0;i<array.num;++i){auto* row=static_cast<Object*>(array.data)+static_cast<size_t>(i)*288;
            now.configs.push_back(ue::At<Object*>(row,0));now.interfaces.push_back(ue::At<uintptr_t>(row,8));}
        ue::Call committed(component,"GetCurrentWeaponIndex"),pending(component,"GetCurrentWeaponIndex"),armed(component,"IsArmed"),transition(component,"IsInWeaponTransition");
        if(!committed.Set("SlotTag",now.tag).Set("WithPending",false).Invoke()||!pending.Set("SlotTag",now.tag).Set("WithPending",true).Invoke()||
           !armed.Set("SlotTag",now.tag).Invoke()||!transition.Invoke())return false;
        now.committed=committed.Get<int32_t>("ReturnValue");now.pending=pending.Get<int32_t>("ReturnValue");now.armed=armed.Get<bool>("ReturnValue");now.transition=transition.Get<bool>("ReturnValue");
        if(now.committed!=ue::At<int32_t>(found,16))return false;output=std::move(now);return true;
    }
    bool Armed(Object* component,Name tag,bool& value) {
        ue::Call c(component,"IsArmed");if(!c.Set("SlotTag",tag).Invoke())return false;value=c.Get<bool>("ReturnValue");return true;
    }
    bool SetIndex(Object* component,Name tag,int index) {
        ue::Call c(component,"SetCurrentWeapon");return c.Set("SlotTag",tag).Set("Index",index).Invoke()&&c.Get<bool>("ReturnValue");
    }
    bool SetArmed(Object* component,Name tag,bool armed){return ue::Call(component,"SetArmed").Set("SlotTag",tag).Set("Armed",armed).Invoke();}
};
} // Detail
inline bool Prepare(std::wstring& reason){return Detail::Prepare(reason);}
inline bool Capture(Object* source,Object* victim,const Policy& sp,const Policy& vp,Ledger& ledger,std::wstring& reason) {
    if(!sp.Any()&&!vp.Any()){Detail::NativeIO io;return CaptureWith(io,source,victim,sp,vp,ledger,reason);}
    if(Detail::metadata.base!=ue::Base()||!Detail::metadata.base)return Reject(reason,L"原生状态恢复接口尚未预载；请等待预载完成");
    Detail::NativeIO io;return CaptureWith(io,source,victim,sp,vp,ledger,reason);
}
inline bool VerifyOrRestore(Ledger& ledger,std::wstring& reason,bool native_ended=true) {
    Detail::NativeIO io;return VerifyOrRestoreWith(io,ledger,reason,native_ended);
}
} // GrabStateLeases
