#include "bone_motion.h"
#include "npc_retarget.h"
#include "boss_assist.h"
#include "settings_store.h"
#include "bone_supplement.h"
#include "bone_collision.h"
#include <windows.h>
#include <memory>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <sstream>

namespace BoneMotionRuntime {
namespace {
using ue::Object;using ue::Name;using ue::Array;using ue::Call;using BoneMotion::Transform;using BoneMotion::Config;
Logger logger_=nullptr;SRWLOCK lock_=SRWLOCK_INIT;Config global_,preview_;Snapshot snapshot_;DWORD game_thread_=0;
bool debug_=false;
bool supplement_=false;
volatile LONG collision_probe_=0;
// Pose listeners: slot 0 is the 身材 / 骨骼 editor's (Listen; the editor's note and status follow it), the others are AddListener's
// (the plugin API's pose event). Each slot has its component and its function; a finalize of a component runs every slot that
// names it, so one listener never replaces another. `listening_` / `listen_faults_` are slot 0's: whether its hook is in, its faults.
constexpr int kListeners=4;
std::atomic<Object*> listen_target_[kListeners]{};std::atomic<PoseListener> listener_[kListeners]{};volatile LONG listening_=0,listen_faults_=0;
char listen_note_[200]="off";
bool global_retired_logged_=false;
constexpr size_t kSlot=380;constexpr uintptr_t kFinalize=0x3dadf30,kFlip=0x3dae6d0;
void Log(const char* format,...){if(!logger_)return;char text[950]{};va_list args;va_start(args,format);_vsnprintf_s(text,sizeof(text),_TRUNCATE,format,args);va_end(args);logger_(text);}
bool Readable(const void* p,size_t n){MEMORY_BASIC_INFORMATION m{};if(!p||!VirtualQuery(p,&m,sizeof(m))||m.State!=MEM_COMMIT||(m.Protect&(PAGE_NOACCESS|PAGE_GUARD)))return false;
    const uintptr_t at=reinterpret_cast<uintptr_t>(p),base=reinterpret_cast<uintptr_t>(m.BaseAddress);return at>=base&&n<=m.RegionSize&&at-base<=m.RegionSize-n;}
Object* Property(Object* o,const char* field){if(!ue::Alive(o))return nullptr;const int32_t at=ue::Offset(ue::ClassOf(o),field);return at>=0?ue::At<Object*>(o,at):nullptr;}
std::string Text(Object* o){char s[200]{};if(o&&ue::Alive(o))ue::NameText(ue::NameOf(o),s,sizeof(s));return s;}
bool Pins(){
    static constexpr unsigned char finalize[]={0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9,0xe8,0x92,0x07,0,0,0x80,0x8b,0xd7,0x07,0,0,0x01,0x48,0x8b,0xcb,0xe8,0x33,0xc4,0xff,0xff,0x48,0x8d,0x8b,0xe0,0x0f,0,0,0x48,0x83,0xc4,0x20,0x5b,0xe9,0x32,0xb0,0x4f,0xfd};
    static constexpr unsigned char flip[]={0x40,0x53,0x48,0x83,0xec,0x20,0x0f,0xb6,0x81,0xda,0x07,0,0,0x48,0x8b,0xd9,0xa8,0x02,0x0f,0x84,0xb7,0x01,0,0,0x24,0xfd};
    return ue::Base()&&!std::memcmp(reinterpret_cast<void*>(ue::Base()+kFinalize),finalize,sizeof(finalize))&&!std::memcmp(reinterpret_cast<void*>(ue::Base()+kFlip),flip,sizeof(flip));
}
struct Hook {
    Object* component=nullptr;Object* mesh=nullptr;Name name{},mesh_name{};uintptr_t* original=nullptr,*table=nullptr;
    Config config;std::string key;std::vector<int> parents,protected_roots;int pelvis=-1;std::vector<std::string> names,missing;
    std::vector<Transform> reference,input,output;std::vector<BoneMotion::Binding> bindings;
    BoneMotion::MotionClock clock;uint64_t frames=0;double max_delta=0;bool wanted=false,retire=false,restored=false,fault=false;
};
std::vector<std::unique_ptr<Hook>> hooks_;
bool Same(const Hook& h){return ue::Alive(h.component)&&ue::NameOf(h.component)==h.name&&Property(h.component,"SkinnedAsset")==h.mesh&&ue::Alive(h.mesh)&&ue::NameOf(h.mesh)==h.mesh_name;}
int Named(const Hook& h,const std::string& name){
    for(size_t i=0;i<h.names.size();++i)if(BoneMotion::Supplemental(name)&&!_stricmp(h.names[i].c_str(),("pbz_"+name).c_str()))return static_cast<int>(i);
    for(size_t i=0;i<h.names.size();++i)if(!_stricmp(h.names[i].c_str(),name.c_str()))return static_cast<int>(i);return -1;}
bool Skeleton(Hook& h){
    // The accepted builds share FReferenceSkeleton's final info/pose arrays (also checked by NpcRetarget).
    if(!Readable(h.mesh+0x300,32))return false;const Array info=ue::At<Array>(h.mesh,0x300),pose=ue::At<Array>(h.mesh,0x310);
    if(info.num<1||info.num>2048||info.max<info.num||pose.num!=info.num||pose.max<pose.num||!Readable(info.data,static_cast<size_t>(info.num)*12)||!Readable(pose.data,static_cast<size_t>(pose.num)*96))return false;
    h.reference.assign(static_cast<Transform*>(pose.data),static_cast<Transform*>(pose.data)+pose.num);
    for(int i=0;i<info.num;++i){const auto* at=static_cast<unsigned char*>(info.data)+i*12;Name n;int parent;std::memcpy(&n,at,8);std::memcpy(&parent,at+8,4);
        if((i==0?parent!=-1:parent<0||parent>=i)||!NpcGripMath::Valid(h.reference[static_cast<size_t>(i)]))return false;
        char name[128]{};ue::NameText(n,name,sizeof(name));h.names.push_back(name);h.parents.push_back(parent);}
    h.pelvis=Named(h,"pelvis");for(const char* name:{"thigh_l","thigh_r","ik_foot_root"}){const int b=Named(h,name);if(b>=0)h.protected_roots.push_back(b);}return true;
}
void Bind(Hook& h,const Config& config){const std::string key=BoneMotion::Encode(config);if(key==h.key)return;h.key=key;h.missing.clear();
    std::vector<int> bones;for(const auto& row:config.rows){const int bone=Named(h,row.bone);bones.push_back(bone);if(row.on&&bone<0)h.missing.push_back(row.bone);}
    h.bindings=BoneMotion::Rebind(h.config,h.bindings,config,bones);if(h.config.on!=config.on)h.clock={};h.config=config;
    Log("BONE_MOTION bind mesh=%s rows=%zu missing=%zu enabled=%d",Text(h.mesh).c_str(),h.bindings.size(),h.missing.size(),config.on?1:0);
}
void Apply(Hook& h){
    if(GetCurrentThreadId()!=game_thread_||!Same(h)||h.fault)return;
    // Native Finalize has flipped the fresh pose into the read buffer (0x640).
    const int read=ue::At<int32_t>(h.component,0x640);if(read<0||read>1)return;
    const Array a=ue::At<Array>(h.component,0x5f8+read*16);if(a.num!=static_cast<int>(h.parents.size())||!Readable(a.data,static_cast<size_t>(a.num)*96))return;
    const size_t bytes=static_cast<size_t>(a.num)*96;std::vector<Transform> input(static_cast<Transform*>(a.data),static_cast<Transform*>(a.data)+a.num);
    // Cached/paused animations can reuse the already modified pose. Recover its baseline before another evaluation.
    if(h.output.size()==input.size()&&h.input.size()==input.size()&&!std::memcmp(input.data(),h.output.data(),bytes))input=h.input;
    if(h.retire||!h.config.on){if(h.output.size()==input.size()){std::memcpy(a.data,input.data(),bytes);BoneCollision::Sync(h.component,a);}BoneSupplement::SyncCollisions(h.component,a);h.restored=true;return;}
    Call get(h.component,"K2_GetComponentToWorld");if(!get.Invoke())return;const Transform world=get.Get<Transform>("ReturnValue");
    static const double frequency=[]{LARGE_INTEGER f{};QueryPerformanceFrequency(&f);return static_cast<double>(f.QuadPart>0?f.QuadPart:1);}();
    LARGE_INTEGER now{};QueryPerformanceCounter(&now);const double dt=h.clock.Advance(static_cast<double>(now.QuadPart)/frequency);
    std::vector<Transform> output;
    if(!BoneMotion::Apply(h.config,h.parents,h.reference,input,world,h.bindings,h.protected_roots,h.pelvis,dt,output))return;
    for(size_t i=0;i<input.size();++i){double d=0;for(size_t k=0;k<3;++k)d+=std::pow(output[i].translation[k]-input[i].translation[k],2);h.max_delta=(std::max)(h.max_delta,std::sqrt(d));}
    h.input=std::move(input);h.output=std::move(output);std::memcpy(a.data,h.output.data(),bytes);++h.frames;
    BoneCollision::Sync(h.component,a);
    BoneSupplement::SyncCollisions(h.component,a);
}
void GuardApply(Hook* h){__try{Apply(*h);}__except(EXCEPTION_EXECUTE_HANDLER){h->fault=true;Log("BONE_MOTION fault: disabled component");}}
void GuardListen(PoseListener listener,Object* component){__try{listener(component);}__except(EXCEPTION_EXECUTE_HANDLER){InterlockedIncrement(&listen_faults_);}}
// After the listener wrote, the pose this module compares a cached frame with is the final one (Apply's baseline check).
// Only while this module edits (Apply wrote this frame): with its edits off the old output must stay as it was.
void KeepFinal(Hook& h){
    if(GetCurrentThreadId()!=game_thread_||h.output.empty()||!h.config.on||h.retire||h.fault||!Same(h))return;
    const int read=ue::At<int32_t>(h.component,0x640);if(read<0||read>1)return;
    const Array a=ue::At<Array>(h.component,0x5f8+read*16);const size_t bytes=h.output.size()*sizeof(Transform);
    if(a.num==static_cast<int>(h.output.size())&&Readable(a.data,bytes))std::memcpy(h.output.data(),a.data,bytes);
}
void GuardKeepFinal(Hook* h){__try{KeepFinal(*h);}__except(EXCEPTION_EXECUTE_HANDLER){}}
void __fastcall Finalize(Object* component){
    Hook* found=nullptr;for(auto& h:hooks_)if(h->component==component){found=h.get();break;}
    if(!found){reinterpret_cast<void(__fastcall*)(Object*)>(ue::Base()+kFinalize)(component);return;}
    const auto original=reinterpret_cast<void(__fastcall*)(Object*)>(found->original[kSlot]);
    const bool fresh=(ue::At<uint8_t>(component,0x7da)&2)!=0;
    original(component);
    if(fresh)for(auto& h:hooks_)if(h.get()==found){GuardApply(h.get());break;}
    bool listened=false;
    for(int i=0;i<kListeners;++i)
        if(component==listen_target_[i].load(std::memory_order_acquire))
            if(const PoseListener listener=listener_[i].load(std::memory_order_acquire)){GuardListen(listener,component);listened=true;}
    if(listened&&fresh)GuardKeepFinal(found);
}
void Remove(Hook& h){
    if(Readable(h.component,sizeof(void*))&&ue::At<uintptr_t*>(h.component,0)==h.table)
        InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(h.component),h.original,h.table);
    ue::ReleaseReference(h.component); // table allocation stays valid until process exit, like the engine tick hook
}
Hook* Ensure(Object* component,const Config& config,bool collision_only=false){
    if(!ue::Alive(component))return nullptr;
    for(auto& h:hooks_)if(h->component==component&&Same(*h)){h->wanted=config.on||collision_only;h->retire=!h->wanted;if(h->wanted)h->restored=false;Bind(*h,config);return h.get();}
    if((!collision_only&&(!config.on||config.rows.empty()))||hooks_.size()>=8||!Pins())return nullptr;
    Object* mesh=Property(component,"SkinnedAsset");if(!ue::Alive(mesh))return nullptr;
    uintptr_t* original=ue::At<uintptr_t*>(component,0);if(!Readable(original,(kSlot+1)*8)||original[kSlot]!=ue::Base()+kFinalize)return nullptr;
    size_t count=0;while(count<1024&&Readable(original+count,8)&&original[count]>=ue::Base()&&original[count]<ue::Base()+0x10000000)++count;
    if(count<=kSlot||count==1024)return nullptr;auto h=std::make_unique<Hook>();h->component=component;h->name=ue::NameOf(component);h->mesh=mesh;h->mesh_name=ue::NameOf(mesh);h->original=original;
    if(!Skeleton(*h)||!ue::AddReference(component))return nullptr;
    auto* allocation=static_cast<uintptr_t*>(VirtualAlloc(nullptr,(count+1)*8,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    if(!allocation){ue::ReleaseReference(component);return nullptr;}std::memcpy(allocation,original-1,(count+1)*8);h->table=allocation+1;h->table[kSlot]=reinterpret_cast<uintptr_t>(&Finalize);
    h->wanted=true;Bind(*h,config);Hook* result=h.get();hooks_.push_back(std::move(h));
    if(InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(component),result->table,original)!=original){Remove(*result);hooks_.pop_back();return nullptr;}
    Log("BONE_MOTION hooked component=%s mesh=%s bones=%zu slot=%zu entries=%zu",Text(component).c_str(),Text(mesh).c_str(),result->names.size(),kSlot,count);return result;
}
void TickImpl(bool transitioning){
    game_thread_=GetCurrentThreadId();Config global,preview;
    bool debug=false,supplement=false;AcquireSRWLockShared(&lock_);global=global_;preview=preview_;debug=debug_;supplement=supplement_;ReleaseSRWLockShared(&lock_);
    BoneSupplement::Begin(supplement,transitioning,BeforeComponentHook);
    for(auto& h:hooks_)h->wanted=false;
    Hook* primary=nullptr;Hook* secondary=nullptr;
    static Object* listed_mesh=nullptr;static Name listed_name{};static std::vector<std::string> listed_bones;
    const auto bind_body=[&](Object* source,const Config& config){
        Object* display=supplement?BoneSupplement::Ensure(source):source;
        std::array<Hook*,2> result{};
        if(display==source)result[0]=Ensure(source,config);
        else {const auto split=BoneMotion::SplitSupplement(config);result[0]=Ensure(source,split[0]);result[1]=Ensure(display,split[1],true);}
        return result;
    };
    // The 骨骼变换 page's own (global) edits are retired: the 身材 / 骨骼 editor took its place (2026-11-13). Only the 骨骼变换
    // nodes of a chain and their preview still edit here.
    if(global.on&&!global_retired_logged_){global_retired_logged_=true;Log("BONE_MOTION saved page edits ignored: the 身材 / 骨骼 editor replaced the page");}
    if(!transitioning){const Config node=NpcRetarget::BoneEdits();const auto bound=bind_body(NpcRetarget::BoneBody(),node);primary=bound[0];secondary=bound[1];
        Object* boss=BossAssist::PreviewBoss();if(ue::Alive(boss))bind_body(Property(boss,"Mesh"),preview);}
    // The editor's component keeps a hook while it listens (after the bodies above: their Ensure may have retired it).
    {Object* listen=nullptr;bool in=false;
        for(int slot=0;slot<kListeners;++slot){
            Object* target=listener_[slot].load()?listen_target_[slot].load():nullptr;bool hooked=false;
            if(!transitioning&&target&&ue::Alive(target)){Hook* h=nullptr;for(auto& x:hooks_)if(x->component==target&&Same(*x)){h=x.get();break;}
                if(!h)h=Ensure(target,Config{},true);
                if(h){h->wanted=true;h->retire=false;hooked=true;}}
            if(slot==0){listen=target;in=hooked;}}
        if(InterlockedExchange(&listening_,in?1:0)!=(in?1:0)||!in){
            if(in)_snprintf_s(listen_note_,sizeof(listen_note_),_TRUNCATE,"pose hook on %s (shared with the 骨骼变换 node), faults=%ld",Text(listen).c_str(),listen_faults_);
            else _snprintf_s(listen_note_,sizeof(listen_note_),_TRUNCATE,"%s",!listen?"off":transitioning?"waiting: scene change":
                             hooks_.size()>=8?"waiting: every hook slot is taken":!Pins()?"no hook: this game build's pose code differs":"waiting for the hero's body");}}
    BoneSupplement::End(BeforeComponentHook);
    if(InterlockedExchange(&collision_probe_,0))BoneSupplement::ProbeCollisions();
    for(auto it=hooks_.begin();it!=hooks_.end();){Hook& h=**it;
        if(transitioning||!Same(h)||(h.retire&&h.restored)){if(primary==&h)primary=nullptr;if(secondary==&h)secondary=nullptr;Remove(h);it=hooks_.erase(it);}
        else{if(!h.wanted)h.retire=true;++it;}}
    Snapshot s;s.hooks=static_cast<int>(hooks_.size());s.supplement_enabled=supplement;s.supplement_displays=static_cast<int>(BoneSupplement::entries.size());s.supplement_note=BoneSupplement::note;
    s.note="Disabled";
    for(Hook* h:{primary,secondary})if(h){s.missing.insert(s.missing.end(),h->missing.begin(),h->missing.end());s.frames=(std::max)(s.frames,h->frames);s.max_delta=(std::max)(s.max_delta,h->max_delta);
        for(const auto& binding:h->bindings)if(binding.bone>=0)++s.matched;
        if(h->fault)s.note="Component disabled after fault";else if(h->config.on&&s.note!="Component disabled after fault")s.note="Active";}
    Object* catalog_body=BoneSupplement::Display(NpcRetarget::BoneBody());Object* catalog_mesh=Property(catalog_body,"SkinnedAsset");
    if(ue::Alive(catalog_mesh)){if(catalog_mesh!=listed_mesh||!(ue::NameOf(catalog_mesh)==listed_name)){Hook h;h.mesh=catalog_mesh;if(Skeleton(h)){listed_mesh=catalog_mesh;listed_name=ue::NameOf(catalog_mesh);listed_bones=std::move(h.names);}}s.mesh=Text(catalog_mesh);s.bones=listed_bones;}
    if(debug&&!transitioning){
        static Object* mesh_cached=nullptr;static Name mesh_name{};static Hook skeleton;static ULONGLONG sampled=0;static uint64_t serial=0;
        static std::vector<std::array<double,3>> positions;Object* body=NpcRetarget::BoneBody();Object* boss=BossAssist::PreviewBoss();
        if(ue::Alive(boss)){Object* target=Property(boss,"Mesh");if(ue::Alive(target)){body=target;s.debug_preview=true;}}
        body=BoneSupplement::Display(body);Object* mesh=Property(body,"SkinnedAsset");
        if(ue::Alive(mesh)){
            if(mesh!=mesh_cached||!(ue::NameOf(mesh)==mesh_name)){skeleton={};skeleton.mesh=mesh;positions.clear();mesh_cached=nullptr;
                if(Skeleton(skeleton)){mesh_cached=mesh;mesh_name=ue::NameOf(mesh);sampled=0;}}
            if(mesh_cached==mesh&&GetTickCount64()>=sampled){sampled=GetTickCount64()+33;
                const int read=ue::At<int32_t>(body,0x640);if(read>=0&&read<=1){const Array a=ue::At<Array>(body,0x5f8+read*16);
                    if(a.num==static_cast<int>(skeleton.names.size())&&Readable(a.data,static_cast<size_t>(a.num)*sizeof(Transform))){
                        positions.clear();for(int i=0;i<a.num;++i){const Transform& t=static_cast<const Transform*>(a.data)[i];if(!NpcGripMath::FiniteMount(t)){positions.clear();break;}positions.push_back({t.translation[0],t.translation[1],t.translation[2]});}++serial;}}}
            s.debug=true;s.debug_frames=serial;s.mesh=Text(mesh);s.bones=skeleton.names;s.parents=skeleton.parents;s.positions=positions;s.reference=skeleton.reference;
        }
    }
    AcquireSRWLockExclusive(&lock_);snapshot_=std::move(s);ReleaseSRWLockExclusive(&lock_);
}
}
void Initialize(Logger logger){logger_=logger;const std::wstring saved=SettingsStore::Get(L"BoneMotion",L"Config");
    BoneSupplement::logger=logger;BoneSupplement::release_source=BeforeComponentHook;supplement_=SettingsStore::Get(L"BoneMotion",L"Supplement")==L"1";
    const int n=WideCharToMultiByte(CP_UTF8,0,saved.data(),static_cast<int>(saved.size()),nullptr,0,nullptr,nullptr);std::string text(static_cast<size_t>(n),' ');
    if(n)WideCharToMultiByte(CP_UTF8,0,saved.data(),static_cast<int>(saved.size()),text.data(),n,nullptr,nullptr);global_=BoneMotion::Decode(text);}
void Tick(bool transitioning){__try{TickImpl(transitioning);}__except(EXCEPTION_EXECUTE_HANDLER){Log("BONE_MOTION tick fault");}}
void SetGlobal(const Config& config,bool save){const Config clean=BoneMotion::Clean(config);AcquireSRWLockExclusive(&lock_);global_=clean;ReleaseSRWLockExclusive(&lock_);
    if(save){const std::string text=BoneMotion::Encode(clean);const int n=MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0);std::wstring wide(static_cast<size_t>(n),L' ');if(n)MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),wide.data(),n);SettingsStore::Put(L"BoneMotion",L"Config",wide);}}
Config Global(){AcquireSRWLockShared(&lock_);Config c=global_;ReleaseSRWLockShared(&lock_);return c;}
void Preview(const Config& config){AcquireSRWLockExclusive(&lock_);preview_=BoneMotion::Clean(config);ReleaseSRWLockExclusive(&lock_);}
bool BeforeDormancy(Object* component,bool(*quiesce)(Object*)){
    if(!quiesce||!quiesce(component))return false;
    for(auto it=BoneSupplement::entries.begin();it!=BoneSupplement::entries.end();){
        if(it->source!=component){++it;continue;}
        if(BoneSupplement::Same(it->display,it->display_name)&&!quiesce(it->display))return false;
        BoneSupplement::Drop(*it,BeforeComponentHook);it=BoneSupplement::entries.erase(it);
    }
    BeforeComponentHook(component);return true;
}
void BeforeComponentHook(Object* component){
    for(auto it=hooks_.begin();it!=hooks_.end();){Hook& h=**it;if(h.component!=component){++it;continue;}
        // A native-only vtable copier must never stop at this private table's DLL entry.
        if(Same(h)&&h.input.size()==h.output.size())for(int buffer=0;buffer<2;++buffer){const Array a=ue::At<Array>(component,0x5f8+buffer*16);const size_t bytes=h.output.size()*sizeof(Transform);
            if(a.num==static_cast<int>(h.output.size())&&Readable(a.data,bytes)&&!std::memcmp(a.data,h.output.data(),bytes))std::memcpy(a.data,h.input.data(),bytes);}
        if(Same(h)){const int read=ue::At<int32_t>(component,0x640);if(read>=0&&read<=1){const Array a=ue::At<Array>(component,0x5f8+read*16);if(a.num==static_cast<int>(h.parents.size()))BoneCollision::Sync(component,a);}}
        Remove(h);it=hooks_.erase(it);Log("BONE_MOTION released before component adapter installation");
    }
}
Snapshot Get(){AcquireSRWLockShared(&lock_);Snapshot s=snapshot_;ReleaseSRWLockShared(&lock_);return s;}
namespace {
// Target first when stopping (a finalize running now sees no target), last when starting (its listener is set).
void SetSlot(int slot,Object* component,PoseListener listener){
    if(!listener||!component){listen_target_[slot].store(nullptr,std::memory_order_release);listener_[slot].store(nullptr,std::memory_order_release);return;}
    listener_[slot].store(listener,std::memory_order_release);listen_target_[slot].store(component,std::memory_order_release);
}
}
void Listen(Object* component,PoseListener listener){SetSlot(0,component,listener);}
int AddListener(Object* component,PoseListener listener){
    if(!component||!listener)return 0;
    for(int slot=1;slot<kListeners;++slot)if(!listener_[slot].load(std::memory_order_acquire)){SetSlot(slot,component,listener);return slot;}
    return 0;
}
void RemoveListener(int id){if(id>0&&id<kListeners)SetSlot(id,nullptr,nullptr);}
bool Listening(){return InterlockedCompareExchange(&listening_,0,0)!=0;}
const char* ListenNote(){return listen_note_;}
void Debug(bool enabled){AcquireSRWLockExclusive(&lock_);debug_=enabled;ReleaseSRWLockExclusive(&lock_);}
void Supplement(bool enabled,bool save){AcquireSRWLockExclusive(&lock_);supplement_=enabled;ReleaseSRWLockExclusive(&lock_);if(save)SettingsStore::Put(L"BoneMotion",L"Supplement",enabled?L"1":L"0");}
bool SupplementEnabled(){AcquireSRWLockShared(&lock_);const bool enabled=supplement_;ReleaseSRWLockShared(&lock_);return enabled;}
bool Command(const std::string& text){std::istringstream in(text);std::string action;in>>action;
    if(action=="collisionprobe"){InterlockedExchange(&collision_probe_,1);return true;}
    if(action=="supplement"){std::string value;in>>value;if(value=="on"||value=="off"){Supplement(value=="on",false);return true;}return false;}
    if(action=="debug"){std::string value;in>>value;if(value!="on"&&value!="off")return false;Debug(value=="on");return true;}
    if(action=="status"){const Snapshot s=Get();Log("BONE_MOTION status mesh=%s hooks=%d matched=%d missing=%zu frames=%llu max_delta=%.4f note=%s",s.mesh.c_str(),s.hooks,s.matched,s.missing.size(),static_cast<unsigned long long>(s.frames),s.max_delta,s.note.c_str());for(const auto& n:s.missing)Log("BONE_MOTION missing %s",n.c_str());
        size_t body=0;for(const auto& n:s.bones)if(!BoneMotion::Facial(n))++body;Log("BONE_DEBUG enabled=%d preview=%d frames=%llu bones=%zu body=%zu positions=%zu parents=%zu",s.debug?1:0,s.debug_preview?1:0,static_cast<unsigned long long>(s.debug_frames),s.bones.size(),body,s.positions.size(),s.parents.size());return true;}
    if(action=="preset"){SetGlobal(BoneMotion::Feminine(),false);return true;}
    if(action=="off"||action=="on"){Config c=Global();c.on=action=="on";SetGlobal(c,false);return true;}
    if(action=="set"){Config c;c.on=true;c.protect_feet=false;BoneMotion::Row r;if(!(in>>r.bone))return false;for(double& v:r.translation)in>>v;for(double& v:r.rotation)in>>v;for(double& v:r.scale)in>>v;if(!in)return false;c.rows.push_back(r);SetGlobal(c,false);return true;}
    return false;
}
}
