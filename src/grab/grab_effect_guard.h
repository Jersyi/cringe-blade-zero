#pragma once
#include "grab_state_leases.h"

// Read-only effect guard for native actions whose deferred cleanup removes an
// effect by class. The caller pins the class, owns exact ability receipts and
// waits for native End, foreign-execution exclusion and deferred world ticks.
// This helper never removes effects, changes health, or restores a raw field.
namespace GrabEffectGuard {
using ue::Object;using Identity=GrabStateLeases::Identity;
struct Side {Identity actor,asc;bool retired=false;};
struct Ledger {
    Identity world,effect;Side source,victim;
    bool pending=false,preserved=false;
    bool HasPending()const{return pending;}
};
inline bool Reject(std::wstring& reason,const wchar_t* text){reason=text;return false;}
template<class IO>bool CaptureWith(IO& io,Object* source,Object* source_asc,Object* victim,Object* victim_asc,Object* world_expected,Object* effect,Ledger& ledger,std::wstring& reason){
    reason.clear();if(ledger.HasPending())return Reject(reason,L"锁血效果上一次检查尚未完成");
    Ledger next;next.world=io.Identify(world_expected);next.effect=io.Identify(effect);next.source.actor=io.Identify(source);next.source.asc=io.Identify(source_asc);next.victim.actor=io.Identify(victim);next.victim.asc=io.Identify(victim_asc);
    if(source==victim||source_asc==victim_asc||!io.Same(next.world)||!io.Same(next.effect))return Reject(reason,L"锁血效果检查对象或世界无效");
    for(Side* side:{&next.source,&next.victim}){
        if(!io.Same(side->actor)||!io.Same(side->asc))return Reject(reason,L"锁血效果检查角色或能力系统无效");
        Object* actual_world=nullptr,*actual_asc=nullptr;
        if(!io.World(side->actor.object,actual_world)||actual_world!=world_expected||!io.ASC(side->actor.object,actual_asc)||actual_asc!=side->asc.object)return Reject(reason,L"锁血效果检查世界或能力系统读回不符");
        int count=-1;if(!io.Count(side->asc.object,effect,count)||count<0)return Reject(reason,L"无法读取原生锁血效果数量，未施放");
        if(count)return Reject(reason,L"角色已有同类锁血效果；保留原生状态，未施放");
    }
    next.pending=true;ledger=next;return true;
}
template<class IO>bool VerifyWith(IO& io,Ledger& ledger,std::wstring& reason,bool native_ended,bool deferred_settled){
    reason.clear();if(!ledger.HasPending())return true;
    if(!native_ended||!deferred_settled)return Reject(reason,L"等待原生投技及延迟清理完成后检查锁血效果");
    if(!io.Same(ledger.world)){ledger.pending=false;ledger.preserved=true;return true;}
    if(!io.Same(ledger.effect))return Reject(reason,L"锁血效果类身份已变化，保留清理记录");
    bool complete=true;
    for(Side* side:{&ledger.source,&ledger.victim}){
        if(side->retired)continue;
        if(!io.Same(side->actor)||!io.Same(side->asc)){side->retired=true;ledger.preserved=true;continue;}
        Object* actual_world=nullptr,*actual_asc=nullptr;
        if(!io.World(side->actor.object,actual_world)||!io.ASC(side->actor.object,actual_asc)){complete=false;reason=L"等待锁血效果角色与世界读回";continue;}
        if(actual_world!=ledger.world.object||actual_asc!=side->asc.object){side->retired=true;ledger.preserved=true;continue;}
        int count=-1;
        if(!io.Count(side->asc.object,ledger.effect.object,count)||count<0){complete=false;reason=L"等待原生锁血效果数量读回";continue;}
        if(count){complete=false;reason=L"原生锁血效果尚未清除；保留记录，等待原生清理";continue;}
    }
    if(!complete)return false;ledger.pending=false;return true;
}
namespace Detail {
using GrabStateLeases::Detail::Field;using GrabStateLeases::Detail::Shape;using GrabStateLeases::Detail::Function;using GrabStateLeases::Detail::Readable;
struct Metadata {uintptr_t base=0;Object* effect=nullptr,*asc=nullptr,*world=nullptr,*level=nullptr,*library=nullptr,*class_type=nullptr;};
inline Metadata metadata;
inline bool Prepare(Object* effect,std::wstring& reason){
    Metadata m;m.base=ue::Base();m.class_type=ue::FindObject("/Script/CoreUObject.Class");m.asc=ue::FindObject("/Script/GameplayAbilities.AbilitySystemComponent");m.world=ue::FindObject("/Script/Engine.World");m.level=ue::FindObject("/Script/Engine.Level");m.library=ue::Default("/Script/GameplayAbilities.Default__AbilitySystemBlueprintLibrary");Object* base_effect=ue::FindObject("/Script/GameplayAbilities.GameplayEffect");
    if(!m.base||!m.class_type||!m.asc||!m.world||!m.level||!m.library||!base_effect||!ue::Alive(effect)||!ue::IsA(effect,m.class_type))return Reject(reason,L"锁血效果原生接口资源尚未就绪");
    bool derived=false;int depth=0;for(Object* type=effect;ue::Alive(type)&&++depth<64;type=ue::At<Object*>(type,0x40)){if(type==base_effect){derived=true;break;}if(!ue::IsA(type,m.class_type))break;}
    if(!derived)return Reject(reason,L"锁血检查对象不是原生 GameplayEffect 类");
    if(!Function(m.asc,"GetGameplayEffectCount",24,{{"SourceGameplayEffect",0x400,8,0},{"OptionalInstigatorFilterComponent",0x10000,8,8},{"bEnforceOnGoingCheck",0x20000,1,16},{"ReturnValue",0x80,4,20}})||!Function(ue::ClassOf(m.library),"GetAbilitySystemComponent",16,{{"Actor",0x10000,8,0},{"ReturnValue",0x10000,8,8}}))return Reject(reason,L"锁血效果原生数量接口参数布局不符");
    m.effect=effect;metadata=m;return true;
}
struct NativeIO {
    Identity Identify(Object* object){return GrabStateLeases::Detail::Identify(object);}bool Same(const Identity& identity){return GrabStateLeases::Detail::Same(identity);}
    bool World(Object* actor,Object*& output){for(int i=0;ue::Alive(actor)&&i<32;++i,actor=ue::OuterOf(actor)){if(ue::IsA(actor,metadata.world)){output=actor;return true;}if(ue::IsA(actor,metadata.level)){Object* property=Field(ue::ClassOf(actor),"OwningWorld");if(!Shape(property,0x10000,8))return false;const int offset=ue::At<int32_t>(property,0x44);if(offset<0||!Readable(actor+offset,8))return false;output=ue::At<Object*>(actor,offset);return ue::Alive(output)&&ue::IsA(output,metadata.world);}}return false;}
    bool ASC(Object* actor,Object*& result){ue::Call call(metadata.library,"GetAbilitySystemComponent");if(!call.Set("Actor",actor).Invoke())return false;result=call.Get<Object*>("ReturnValue");return ue::Alive(result)&&ue::IsA(result,metadata.asc);}
    bool Count(Object* asc,Object* effect,int& result){if(!ue::Alive(asc)||!ue::IsA(asc,metadata.asc)||!ue::Alive(effect))return false;Object* no_filter=nullptr;ue::Call call(asc,"GetGameplayEffectCount");if(!call.Set("SourceGameplayEffect",effect).Set("OptionalInstigatorFilterComponent",no_filter).Set("bEnforceOnGoingCheck",false).Invoke())return false;result=call.Get<int32_t>("ReturnValue");return result>=0&&result<=1048576;}
};
}
inline bool Prepare(Object* effect,std::wstring& reason){return Detail::Prepare(effect,reason);}
inline bool Capture(Object* source,Object* source_asc,Object* victim,Object* victim_asc,Object* world_expected,Object* effect,Ledger& ledger,std::wstring& reason){if(Detail::metadata.base!=ue::Base()||!Detail::metadata.base||Detail::metadata.effect!=effect)return Reject(reason,L"锁血效果检查必须先完成预载");Detail::NativeIO io;return CaptureWith(io,source,source_asc,victim,victim_asc,world_expected,effect,ledger,reason);}
inline bool Verify(Ledger& ledger,std::wstring& reason,bool native_ended,bool deferred_settled){Detail::NativeIO io;return VerifyWith(io,ledger,reason,native_ended,deferred_settled);}
}
