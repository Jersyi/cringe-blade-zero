#pragma once
#include "grab_state_leases.h"
#include <array>

// Exact HuaShan troop grab only. No gameplay work occurs unless the catalog's
// huashan_domain_bridge flag is selected. Caller holds exact pair grant receipts.
// Capture -> native pair + success -> Apply are one uninterrupted GT invocation.
namespace GrabHuashanAdapter {
using ue::Object;using ue::Name;using Identity=GrabStateLeases::Identity;
struct Vector { double x=0,y=0,z=0; };
// Snapshot copied out of ue::Call's native 16-byte-aligned parameter block.
// Its address is never passed to native code; no over-alignment is needed here.
struct Transform { double q[4]{0,0,0,1},t[4]{},s[4]{1,1,1,0}; };
static_assert(sizeof(Vector)==24&&sizeof(Transform)==96,"Native LWC layouts");
struct PairInfo {GrabStateLeases::Weak executor,target;float start=0;uint8_t is_target=0,pad[3]{};int32_t source_handle=0,victim_handle=0;};
static_assert(sizeof(PairInfo)==32,"Native execution-info layout");
// Validate the RAW authored warp point. Derive a separate capsule center for
// floor/clearance tests; never add a half-height to this point or modify its Z.
using LandingValidator=bool(*)(Object*,Object*,const Vector&,std::wstring&);
struct Ledger {
    Identity source,victim,world,blackboard,warping,source_ga,victim_ga;
    Vector domain,origin,endpoint;Transform native_target,written_target;
    uint64_t captured_at=0;uint32_t thread=0;
    bool captured=false,applied=false,pending=false,native_owned=false,written_owned=false,preserved=false;
    bool HasPending()const{return pending;}bool Applied()const{return applied;}
};
inline bool Reject(std::wstring& reason,const wchar_t* text){reason=text;return false;}
inline bool Finite(Vector v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z)&&std::fabs(v.x)<=1e8&&std::fabs(v.y)<=1e8&&std::fabs(v.z)<=1e8;}
inline double Distance(Vector a,Vector b){return std::hypot(std::hypot(a.x-b.x,a.y-b.y),a.z-b.z);}
inline bool Equal(const Transform& a,const Transform& b){for(int i=0;i<4;++i)if(!std::isfinite(a.q[i]+b.q[i])||std::fabs(a.q[i]-b.q[i])>1e-5)return false;for(int i=0;i<3;++i)if(!std::isfinite(a.t[i]+b.t[i]+a.s[i]+b.s[i])||std::fabs(a.t[i]-b.t[i])>.01||std::fabs(a.s[i]-b.s[i])>1e-5)return false;return true;}
inline Transform AtLocation(Vector point){Transform value;value.t[0]=point.x;value.t[1]=point.y;value.t[2]=point.z;return value;}
inline Vector ClampEndpoint(Vector origin,Vector domain){const double length=Distance(origin,domain),scale=length>800?800/length:1;return {origin.x+(domain.x-origin.x)*scale,origin.y+(domain.y-origin.y)*scale,origin.z+(domain.z-origin.z)*scale};}

template<class IO>bool CaptureWith(IO& io,Object* source,Object* victim,Ledger& ledger,std::wstring& reason){
    reason.clear();if(ledger.HasPending())return Reject(reason,L"场地投摔上一次清理尚未完成");
    Ledger next;next.source=io.Identify(source);next.victim=io.Identify(victim);
    if(source==victim||!io.Same(next.source)||!io.Same(next.victim))return Reject(reason,L"场地投摔双方身份无效");
    Object* sw=nullptr,*vw=nullptr;
    if(!io.World(source,sw)||!io.World(victim,vw)||sw!=vw||(next.world=io.Identify(sw),!io.Same(next.world)))return Reject(reason,L"场地投摔双方不在同一有效世界");
    Object* blackboard=nullptr,*warping=nullptr;Vector origin;
    if(!io.Domain(victim,blackboard,next.domain)||(next.blackboard=io.Identify(blackboard),!io.Same(next.blackboard))||!Finite(next.domain))return Reject(reason,L"受技敌人没有已设置的有效场地中心，未施放");
    if(!io.Position(victim,origin)||!Finite(origin)||Distance(origin,next.domain)>100000)return Reject(reason,L"场地中心与受技者距离异常，未施放");
    next.origin=origin;next.endpoint=ClampEndpoint(origin,next.domain);
    if(!Finite(next.endpoint)||Distance(origin,next.endpoint)>800.001||!io.Landing(source,victim,next.endpoint,reason))return Reject(reason,L"场地投摔落点未通过安全地面或胶囊检查");
    if(!io.Warping(victim,warping)||(next.warping=io.Identify(warping),!io.Same(next.warping)))return Reject(reason,L"受技者原生位移组件不可用");
    bool found=false;Transform old;
    if(!io.GetTarget(warping,found,old))return Reject(reason,L"无法读取原生 TroopThrow 位移目标");
    if(found)return Reject(reason,L"受技者已有 TroopThrow 位移目标；保留原生绑定，未施放");
    next.captured=true;next.pending=true;next.thread=io.Thread();next.captured_at=io.Now();ledger=std::move(next);return true;
}
// Read-only admission immediately after the caller binds exact fresh native
// grants, before source-info refresh/alignment/success can fail. This receipt
// permits cleanup of the receiver's new target even if correction never runs.
template<class IO>bool ObserveWith(IO& io,Ledger& ledger,Object* source_ga,Object* victim_ga,std::wstring& reason){
    reason.clear();if(!ledger.captured||!ledger.pending)return Reject(reason,L"场地投摔缺少本次预检记录");
    if(io.Thread()!=ledger.thread)return Reject(reason,L"场地投摔同步对位窗口已过，已请求取消");
    if(!io.Same(ledger.source)||!io.Same(ledger.victim)||!io.Same(ledger.world)||!io.Same(ledger.blackboard)||!io.Same(ledger.warping))return Reject(reason,L"场地投摔对象代际已变化，已请求取消");
    Object* sw=nullptr,*vw=nullptr,*warping=nullptr;
    if(!io.World(ledger.source.object,sw)||!io.World(ledger.victim.object,vw)||sw!=ledger.world.object||vw!=sw||!io.Warping(ledger.victim.object,warping)||warping!=ledger.warping.object||!io.Abilities(source_ga,victim_ga,ledger.source.object,ledger.victim.object))return Reject(reason,L"场地投摔能力或世界读回不符，已请求取消");
    if((ledger.source_ga.object&&(ledger.source_ga.object!=source_ga||!io.Same(ledger.source_ga)))||(ledger.victim_ga.object&&(ledger.victim_ga.object!=victim_ga||!io.Same(ledger.victim_ga))))return Reject(reason,L"场地投摔能力身份已失效");
    if(!ledger.source_ga.object)ledger.source_ga=io.Identify(source_ga);
    if(!ledger.victim_ga.object)ledger.victim_ga=io.Identify(victim_ga);
    if(!io.Same(ledger.source_ga)||!io.Same(ledger.victim_ga))return Reject(reason,L"场地投摔能力身份已失效");
    bool found=false;Transform current;
    if(!io.GetTarget(warping,found,current)||!found)return Reject(reason,L"原生受技阶段尚未创建 TroopThrow 目标，已请求取消");
    // Exact live pair/world proven above; this target was absent in Capture and
    // was created by the synchronous native receiver. Record BEFORE gates so
    // a timeout/window refusal can still retire our native target after End.
    if(ledger.native_owned&&!Equal(current,ledger.native_target)&&(!ledger.written_owned||!Equal(current,ledger.written_target)))return Reject(reason,L"场地投摔位移目标已被其他系统更改，已请求取消");
    if(!ledger.native_owned){ledger.native_target=current;ledger.native_owned=true;}return true;
}
template<class IO>bool ApplyWith(IO& io,Ledger& ledger,Object* source_ga,Object* victim_ga,std::wstring& reason){
    if(!ObserveWith(io,ledger,source_ga,victim_ga,reason))return false;
    if(ledger.applied)return true; // Never repeat a write or restart movement.
    const uint64_t now=io.Now();
    if(now<ledger.captured_at||now-ledger.captured_at>500)return Reject(reason,L"场地投摔同步对位窗口已过，已请求取消");
    if(!io.Early(victim_ga))return Reject(reason,L"场地投摔原生位移窗口已开始，已请求取消");
    Vector current_origin;
    if(!io.Position(ledger.victim.object,current_origin)||Distance(current_origin,ledger.origin)>2)return Reject(reason,L"场地投摔预检后受技者位置变化，已请求取消");
    if(!io.SetSourceLocation(source_ga,ledger.domain))return Reject(reason,L"场地投摔施技目标修正读回失败，已请求取消");
    ledger.written_target=AtLocation(ledger.endpoint);ledger.written_owned=true;
    io.SetTarget(ledger.warping.object,ledger.endpoint); // Unknown return still needs readback and later cleanup.
    bool found=false;Transform current;
    if(!io.GetTarget(ledger.warping.object,found,current)||!found||!Equal(current,ledger.written_target))return Reject(reason,L"场地投摔原生位移目标修正读回失败，已请求取消");
    ledger.applied=true;return true;
}
// Cleanup-only path for native source CanActivate failure. Caller has already
// bound the exact fresh victim grant from the native component receipt. An
// invalid source handle is preserved verbatim; it never becomes cast success.
template<class IO>bool ObserveVictimWith(IO& io,Ledger& ledger,Object* victim_ga,int32_t source_handle,int32_t victim_handle,std::wstring& reason){
    reason.clear();if(!ledger.captured||!ledger.pending||io.Thread()!=ledger.thread||victim_handle<=0||source_handle < -1||source_handle==victim_handle)return Reject(reason,L"场地投摔部分激活缺少有效原生受技记录");
    if(!io.Same(ledger.source)||!io.Same(ledger.victim)||!io.Same(ledger.world)||!io.Same(ledger.warping))return Reject(reason,L"场地投摔对象代际已变化，已请求取消");
    Object* source_world=nullptr,*victim_world=nullptr,*component=nullptr;
    if(!io.World(ledger.source.object,source_world)||!io.World(ledger.victim.object,victim_world)||source_world!=ledger.world.object||victim_world!=source_world||!io.Warping(ledger.victim.object,component)||component!=ledger.warping.object||!io.Victim(victim_ga,ledger.source,ledger.victim,source_handle,victim_handle))return Reject(reason,L"场地投摔部分受技身份或句柄读回不符");
    if(ledger.victim_ga.object&&(ledger.victim_ga.object!=victim_ga||!io.Same(ledger.victim_ga)))return Reject(reason,L"场地投摔能力身份已失效");
    if(!ledger.victim_ga.object)ledger.victim_ga=io.Identify(victim_ga);
    if(!io.Same(ledger.victim_ga))return Reject(reason,L"场地投摔能力身份已失效");
    bool found=false;Transform current;
    if(!io.GetTarget(component,found,current))return Reject(reason,L"等待场地投摔位移目标读回");
    if(found){if(ledger.native_owned&&!Equal(current,ledger.native_target)&&(!ledger.written_owned||!Equal(current,ledger.written_target)))return Reject(reason,L"场地投摔位移目标已被其他系统更改，已请求取消");if(!ledger.native_owned){ledger.native_target=current;ledger.native_owned=true;}}
    return true;
}
template<class IO>bool FinalizeWith(IO& io,Ledger& ledger,std::wstring& reason,bool native_ended=true){
    reason.clear();if(!ledger.HasPending())return true;
    if(!native_ended)return Reject(reason,L"场地投摔原生动作尚未结束，等待目标清理");
    if(!io.Same(ledger.victim)||!io.Same(ledger.warping)){ledger.pending=false;ledger.preserved=true;return true;}
    if(!io.Same(ledger.world)){ledger.pending=false;ledger.preserved=true;return true;}
    Object* world_now=nullptr;
    if(!io.World(ledger.victim.object,world_now))return Reject(reason,L"等待场地投摔世界读回");
    if(world_now!=ledger.world.object){ledger.pending=false;ledger.preserved=true;return true;}
    Object* component=nullptr;
    if(!io.Warping(ledger.victim.object,component))return Reject(reason,L"等待场地投摔位移组件读回");
    if(component!=ledger.warping.object){ledger.pending=false;ledger.preserved=true;return true;}
    bool found=false;Transform current;
    if(!io.GetTarget(component,found,current))return Reject(reason,L"等待场地投摔位移目标读回");
    if(!found){ledger.pending=false;return true;}
    if((!ledger.native_owned||!Equal(current,ledger.native_target))&&(!ledger.written_owned||!Equal(current,ledger.written_target))){ledger.pending=false;ledger.preserved=true;return true;}
    io.RemoveTarget(component);
    if(!io.GetTarget(component,found,current)||found)return Reject(reason,L"等待场地投摔原生位移目标移除读回");
    ledger.pending=false;return true;
}

namespace Detail {
using GrabStateLeases::Detail::Field;using GrabStateLeases::Detail::Shape;using GrabStateLeases::Detail::Function;using GrabStateLeases::Detail::Readable;
constexpr const char* kSource="/PBZCombatAsset/ActorData/Mobs/035_HuaShan/Abilities/Troop/Grab/GA_HuaShan_Troop_GEx_01.GA_HuaShan_Troop_GEx_01_C";
constexpr const char* kVictim="/PBZCombatAsset/ActorData/Mobs/035_HuaShan/Abilities/Troop/Grab/GA_HuaShan_Troop_GEd_01.GA_HuaShan_Troop_GEd_01_C";
struct Metadata {uintptr_t base=0;Object* source=nullptr,*victim=nullptr,*location=nullptr,*world=nullptr,*level=nullptr,*blackboard=nullptr,*warping=nullptr,*holder=nullptr,*execution_interface=nullptr,*ai_library=nullptr,*bb_library=nullptr,*ability_library=nullptr,*task=nullptr,*time=nullptr,*timeline=nullptr;Name domain_tag,domain_key,troop,execution;int source_location=-1,time_offset=-1,timeline_offset=-1;};
inline Metadata metadata;
inline bool Prepare(std::wstring& reason){
    Metadata m;m.base=ue::Base();m.source=ue::FindObject(kSource);m.victim=ue::FindObject(kVictim);
    m.world=ue::FindObject("/Script/Engine.World");m.level=ue::FindObject("/Script/Engine.Level");m.blackboard=ue::FindObject("/Script/AIModule.BlackboardComponent");m.warping=ue::FindObject("/Script/MotionWarping.MotionWarpingComponent");
    m.ai_library=ue::Default("/Script/SGameAI.Default__SGameAIFunctionLibrary");m.bb_library=ue::Default("/Script/AIModule.Default__AIBlueprintHelperLibrary");m.ability_library=ue::Default("/Script/SGameUtils.Default__AbilityUtils");
    m.task=ue::FindObject("/Script/LegoAbility.LGATask_PlayTimeline");
    m.holder=ue::FindObject("/Script/SGameUtils.CharacterGeneralComponentsHolder");
    m.execution_interface=ue::FindObject("/Script/SGameExecution.ExecutionAbilityInterface");
    if(!m.base||!m.source||!m.victim||!m.world||!m.level||!m.blackboard||!m.warping||!m.ai_library||!m.bb_library||!m.ability_library||!m.task||!m.holder||!m.execution_interface)return Reject(reason,L"场地投摔接口资源尚未预载完整");
    m.location=Field(m.source,"Target Location");Object* vector=ue::FindObject("/Script/CoreUObject.Vector");
    if(!Shape(m.location,0x100000,24)||!vector||ue::StructSize(vector)!=24||ue::At<Object*>(m.location,0x70)!=vector)return Reject(reason,L"场地投摔目标坐标属性布局不符");
    m.source_location=ue::At<int32_t>(m.location,0x44);
    m.time=Field(m.task,"CurrentTime");m.timeline=Field(m.task,"Timeline");
    if(!Shape(m.time,0x100,4)||!Shape(m.timeline,0x10000,8))return Reject(reason,L"场地投摔时间轴读取布局不符");
    m.time_offset=ue::At<int32_t>(m.time,0x44);m.timeline_offset=ue::At<int32_t>(m.timeline,0x44);
    constexpr uint64_t obj=0x10000,st=0x100000,na=0x2000,bo=0x20000;
    if(m.source_location<0x28||m.source_location>ue::StructSize(m.source)-24||m.time_offset<0x28||m.time_offset>ue::StructSize(m.task)-4||m.timeline_offset<0x28||m.timeline_offset>ue::StructSize(m.task)-8||!Function(ue::ClassOf(m.bb_library),"GetBlackboard",16,{{"Target",obj,8,0},{"ReturnValue",obj,8,8}})||
       !Function(ue::ClassOf(m.ai_library),"GetValueAsVectorByTag",40,{{"BlackboardComponent",obj,8,0},{"Tag",st,8,8},{"ReturnValue",st,24,16}})||
       !Function(ue::ClassOf(m.ai_library),"GetBlackboardKeyNameFromTag",16,{{"Tag",st,8,0},{"ReturnValue",na,8,8}})||
       !Function(m.blackboard,"IsVectorValueSet",12,{{"KeyName",na,8,0},{"ReturnValue",bo,1,8}})||
       !Function(m.warping,"AddOrUpdateWarpTargetFromLocation",32,{{"WarpTargetName",na,8,0},{"TargetLocation",st,24,8}})||
       !Function(m.warping,"RemoveWarpTarget",12,{{"WarpTargetName",na,8,0},{"ReturnValue",0x80,4,8}})||
       !Function(m.holder,"GetMotionWarpingComponent",8,{{"ReturnValue",obj,8,0}})||
       !Function(m.source,"GetAvatarActorFromActorInfo",8,{{"ReturnValue",obj,8,0}})||
       !Function(m.victim,"GetAvatarActorFromActorInfo",8,{{"ReturnValue",obj,8,0}})||
       !Function(m.execution_interface,"GetCurrentExecutionInfo",32,{{"Info",st,32,0}})||
       !Function(m.victim,"GetPlayTimelineTask",8,{{"ReturnValue",obj,8,0}})||
       !Function(m.victim,"GetTimelineByName",16,{{"TimelineName",na,8,0},{"ReturnValue",obj,8,8}})||
       !Function(ue::ClassOf(m.ability_library),"GetWarpTarget",128,{{"MotionWarpingComponent",obj,8,0},{"WarpTarget",na,8,8},{"Transform",st,96,16},{"ReturnValue",bo,1,112}}))return Reject(reason,L"场地投摔原生函数参数布局不符");
    m.domain_tag=ue::MakeName(L"AI.BlackboardKey.Vector.DomainCenter");m.troop=ue::MakeName(L"TroopThrow");m.execution=ue::MakeName(L"ExecutionTimeline");
    if(m.domain_tag.None()||m.troop.None()||m.execution.None())return Reject(reason,L"场地投摔原生标签未就绪");
    ue::Call key(m.ai_library,"GetBlackboardKeyNameFromTag");if(!key.Set("Tag",m.domain_tag).Invoke()||(m.domain_key=key.Get<Name>("ReturnValue"),m.domain_key.None()))return Reject(reason,L"场地投摔黑板键未就绪");
    metadata=m;return true;
}
struct NativeIO {
    LandingValidator landing=nullptr;
    Identity Identify(Object* o){return GrabStateLeases::Detail::Identify(o);}bool Same(const Identity& id){return GrabStateLeases::Detail::Same(id);}
    uint64_t Now(){return GetTickCount64();}uint32_t Thread(){return GetCurrentThreadId();}
    bool World(Object* actor,Object*& output){for(int i=0;ue::Alive(actor)&&i<32;++i,actor=ue::OuterOf(actor)){if(ue::IsA(actor,metadata.world)){output=actor;return true;}if(ue::IsA(actor,metadata.level)){Object* p=Field(ue::ClassOf(actor),"OwningWorld");if(!Shape(p,0x10000,8))return false;const int offset=ue::At<int32_t>(p,0x44);if(offset<0||!Readable(actor+offset,8))return false;output=ue::At<Object*>(actor,offset);return ue::Alive(output)&&ue::IsA(output,metadata.world);}}return false;}
    bool Position(Object* actor,Vector& point){ue::Call c(actor,"K2_GetActorLocation");if(!c.Slot("ReturnValue",24)||!c.Invoke())return false;point=c.Get<Vector>("ReturnValue");return Finite(point);}
    bool Domain(Object* actor,Object*& blackboard,Vector& point){
        ue::Call bb(metadata.bb_library,"GetBlackboard");if(!bb.Set("Target",actor).Invoke())return false;blackboard=bb.Get<Object*>("ReturnValue");if(!ue::Alive(blackboard)||!ue::IsA(blackboard,metadata.blackboard))return false;
        ue::Call set(blackboard,"IsVectorValueSet");if(!set.Set("KeyName",metadata.domain_key).Invoke()||!set.Get<bool>("ReturnValue"))return false;
        ue::Call get(metadata.ai_library,"GetValueAsVectorByTag");if(!get.Set("BlackboardComponent",blackboard).Set("Tag",metadata.domain_tag).Invoke())return false;point=get.Get<Vector>("ReturnValue");return Finite(point);
    }
    bool Landing(Object* a,Object* b,const Vector& point,std::wstring& reason){return landing&&landing(a,b,point,reason);}
    bool Warping(Object* actor,Object*& warping){ue::Call c(actor,"GetMotionWarpingComponent","/Script/SGameUtils.CharacterGeneralComponentsHolder");if(!c.Slot("ReturnValue",8)||!c.Invoke())return false;warping=c.Get<Object*>("ReturnValue");return ue::Alive(warping)&&ue::IsA(warping,metadata.warping);}
    bool GetTarget(Object* component,bool& found,Transform& value){ue::Call c(metadata.ability_library,"GetWarpTarget");if(!c.Slot("Transform",96)||!c.Set("MotionWarpingComponent",component).Set("WarpTarget",metadata.troop).Invoke())return false;found=c.Get<bool>("ReturnValue");if(found)value=c.Get<Transform>("Transform");return true;}
    bool Abilities(Object* source,Object* victim,Object* a,Object* b){if(!ue::Alive(source)||!ue::Alive(victim)||ue::IsTemplate(source)||ue::IsTemplate(victim)||ue::ClassOf(source)!=metadata.source||ue::ClassOf(victim)!=metadata.victim)return false;ue::Call sa(source,"GetAvatarActorFromActorInfo"),vb(victim,"GetAvatarActorFromActorInfo");return sa.Invoke()&&vb.Invoke()&&sa.Get<Object*>("ReturnValue")==a&&vb.Get<Object*>("ReturnValue")==b;}
    bool Victim(Object* victim,const Identity& source,const Identity& target,int32_t source_handle,int32_t victim_handle){if(!ue::Alive(victim)||ue::IsTemplate(victim)||ue::ClassOf(victim)!=metadata.victim)return false;ue::Call avatar(victim,"GetAvatarActorFromActorInfo"),get(victim,"GetCurrentExecutionInfo","/Script/SGameExecution.ExecutionAbilityInterface");if(!avatar.Invoke()||avatar.Get<Object*>("ReturnValue")!=target.object||!get.Slot("Info",32)||!get.Invoke())return false;const PairInfo info=get.Get<PairInfo>("Info");return info.is_target&&std::isfinite(info.start)&&std::fabs(info.start)<.001f&&info.executor.index==source.weak.index&&info.executor.serial==source.weak.serial&&info.target.index==target.weak.index&&info.target.serial==target.weak.serial&&info.source_handle==source_handle&&info.victim_handle==victim_handle;}
    bool Early(Object* victim){ue::Call task(victim,"GetPlayTimelineTask"),timeline(victim,"GetTimelineByName");if(!task.Invoke()||!timeline.Set("TimelineName",metadata.execution).Invoke())return false;Object* t=task.Get<Object*>("ReturnValue"),*expected=timeline.Get<Object*>("ReturnValue");if(!ue::Alive(t)||!ue::IsA(t,metadata.task)||!ue::Alive(expected)||Field(ue::ClassOf(t),"CurrentTime")!=metadata.time||Field(ue::ClassOf(t),"Timeline")!=metadata.timeline||metadata.time_offset<0||metadata.timeline_offset<0||!Readable(t+metadata.time_offset,4)||!Readable(t+metadata.timeline_offset,8)||ue::At<Object*>(t,metadata.timeline_offset)!=expected)return false;const float value=ue::At<float>(t,metadata.time_offset);return std::isfinite(value)&&value>=0&&value<2.894132375717163f;}
    bool SetSourceLocation(Object* source,Vector point){if(Field(ue::ClassOf(source),"Target Location")!=metadata.location||!Readable(source+metadata.source_location,24))return false;ue::At<Vector>(source,metadata.source_location)=point;return Distance(ue::At<Vector>(source,metadata.source_location),point)<.001;}
    bool SetTarget(Object* component,Vector point){return ue::Call(component,"AddOrUpdateWarpTargetFromLocation").Set("WarpTargetName",metadata.troop).Set("TargetLocation",point).Invoke();}
    bool RemoveTarget(Object* component){return ue::Call(component,"RemoveWarpTarget").Set("WarpTargetName",metadata.troop).Invoke();}
};
}
inline bool Prepare(std::wstring& reason){return Detail::Prepare(reason);}
inline bool Capture(Object* source,Object* victim,Ledger& ledger,std::wstring& reason,LandingValidator landing){if(Detail::metadata.base!=ue::Base()||!Detail::metadata.base)return Reject(reason,L"场地投摔资源必须先完成预载");Detail::NativeIO io;io.landing=landing;return CaptureWith(io,source,victim,ledger,reason);}
inline bool Observe(Ledger& ledger,Object* source,Object* victim,std::wstring& reason){Detail::NativeIO io;return ObserveWith(io,ledger,source,victim,reason);}
inline bool ObserveVictim(Ledger& ledger,Object* victim,int32_t source_handle,int32_t victim_handle,std::wstring& reason){Detail::NativeIO io;return ObserveVictimWith(io,ledger,victim,source_handle,victim_handle,reason);}
inline bool Apply(Ledger& ledger,Object* source,Object* victim,std::wstring& reason){Detail::NativeIO io;return ApplyWith(io,ledger,source,victim,reason);}
inline bool Finalize(Ledger& ledger,std::wstring& reason,bool native_ended=true){Detail::NativeIO io;return FinalizeWith(io,ledger,reason,native_ended);}
}
