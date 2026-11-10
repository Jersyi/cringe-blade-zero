#pragma once
#include "ue_runtime.h"
#include "game_build.h"
#include "bone_motion_model.h"
#include "../generated/bone_supplement_data.generated.h"
#include "../generated/bone_collision_data.generated.h"
#include <filesystem>
#include <vector>
#include <string>

// Game-thread owned display copies. Source meshes, skeletons and their animation drivers are never replaced.
namespace BoneSupplement {
using ue::Object;using ue::Name;using ue::Call;using BoneMotion::Transform;
using Logger=void(*)(const char*);
struct SoftPath {int32_t index=0,serial=0;Name package,asset;ue::String sub;};
struct Bit {int at=-1;uint8_t mask=0;};
struct Collider {Object* object=nullptr;Name name{};int bone=-1;int reserved[3]{};Transform offset;};
struct Entry {Object* source=nullptr,*mesh=nullptr,*display=nullptr,*collision_source=nullptr;Name source_name{},mesh_name{},display_name{},collision_source_name{};bool visible=true,wanted=false;uint8_t tick=0;int tick_at=-1;Bit skin{};bool hidden_skin=false;std::array<Collider,4> colliders;};
inline std::vector<Entry> entries;
inline bool attempted=false,mounted=false;inline std::string note="Disabled";inline Logger logger=nullptr;inline void(*release_source)(Object*)=nullptr;
inline void Log(const std::string& s){if(logger)logger(("BONE_SUPPLEMENT "+s).c_str());}
inline bool Same(Object* o,Name n){return ue::Alive(o)&&ue::NameOf(o)==n;}
inline Object* Property(Object* o,const char* field){if(!ue::Alive(o))return nullptr;const int at=ue::Offset(ue::ClassOf(o),field);return at>=0?ue::At<Object*>(o,at):nullptr;}
inline std::string Text(Object* o){char b[256]{};if(ue::Alive(o))ue::NameText(ue::NameOf(o),b,sizeof(b));return b;}
inline Bit Flag(Object* o,const char* name){for(Object* cls=ue::ClassOf(o);cls;cls=ue::At<Object*>(cls,ue::layout::kSuper)){
    for(Object* field=ue::At<Object*>(cls,ue::layout::kChildProperties);field;field=ue::At<Object*>(field,ue::layout::kPropNext))if(ue::NameIs(ue::At<Name>(field,ue::layout::kPropName),name)){
        const int at=ue::At<int32_t>(field,ue::layout::kPropOffset);const uint8_t byte=ue::At<uint8_t>(field,0x71),mask=ue::At<uint8_t>(field,0x73);
        if(ue::At<uint8_t>(field,0x70)==1&&at>=0&&byte<=7&&mask)return {at+byte,mask};return {};}}
    return {};}
inline bool FlagValue(Object* o,Bit bit){return bit.at>=0&&(ue::At<uint8_t>(o,bit.at)&bit.mask)!=0;}
inline bool HideSkin(Object* o,Bit bit,bool hide){if(bit.at<0)return false;const Bit cast=Flag(o,"CastShadow");if(cast.at<0)return false;
    const bool shadow=FlagValue(o,cast);uint8_t& byte=ue::At<uint8_t>(o,bit.at);byte=static_cast<uint8_t>((byte&~bit.mask)|(hide?bit.mask:0));
    // Both reflected setters schedule render-state recreation; final shadow state is unchanged.
    return Call(o,"SetCastShadow").Set("NewCastShadow",!shadow).Invoke()&&Call(o,"SetCastShadow").Set("NewCastShadow",shadow).Invoke();}
inline bool Visible(Object* o){Call c(o,"IsVisible");return c.Invoke()&&c.Get<bool>("ReturnValue");}
inline bool Show(Object* o,bool v){return Call(o,"SetVisibility").Set("bNewVisibility",v).Set("bPropagateToChildren",false).Invoke();}
inline Object* Load(const std::string& path,bool cls=false){if(Object* o=ue::FindObject(path.c_str()))return o;const auto dot=path.find_last_of('.');if(dot==std::string::npos)return nullptr;
    const std::wstring package(path.begin(),path.begin()+static_cast<std::ptrdiff_t>(dot)),asset(path.begin()+static_cast<std::ptrdiff_t>(dot+1),path.end());SoftPath p;p.package=ue::MakeName(package.c_str());p.asset=ue::MakeName(asset.c_str());
    Call c(ue::Default("/Script/Engine.Default__KismetSystemLibrary"),cls?"LoadClassAsset_Blocking":"LoadAsset_Blocking");
    return c.Set(cls?"AssetClass":"Asset",p).Invoke()?c.Get<Object*>("ReturnValue"):nullptr;}
inline bool Mount(){if(attempted)return mounted;attempted=true;
    HMODULE module=nullptr;wchar_t path[MAX_PATH]{};if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&Mount),&module)||!GetModuleFileNameW(module,path,MAX_PATH))return false;
    auto pak=std::filesystem::path(path).parent_path()/L"PBZSoftBones"/L"PBZSoftBones.pak";
    for(const wchar_t* extension:{L".pak",L".utoc",L".ucas"}){auto p=pak;p.replace_extension(extension);std::error_code error;if(!std::filesystem::exists(p,error)||error){note="PBZSoftBones package is not installed";Log(note);return false;}}
    struct Pin{uintptr_t rva;unsigned char bytes[16];};
    constexpr Pin pins[]={{0x12e0e00,{0x48,0x8d,0x05,0x99,0xfd,0xa3,0x0c,0xc3,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc}},
        {0x12df700,{0xe9,0xcb,0x31,0x6a,0x0e,0xcc,0x24,0xdf,0x62,0xbf,0xef,0x03,0xd3,0x20,0xf1,0xe9}},
        {0x22f8990,{0x40,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x57,0x48,0x8d,0xac,0x24,0x10}}};
    for(const auto& p:pins)if(!GameBuild::CodeMatches(ue::Base()+p.rva,p.bytes,sizeof(p.bytes))){note="Mount code does not match supported build";Log(note);return false;}
    struct Args{const wchar_t* filename=nullptr;uint32_t order=4;const wchar_t* mount=nullptr;uint32_t flags=0;bool index=true;};static_assert(sizeof(Args)==32);
    struct Status{const wchar_t* message=nullptr;uint32_t code=0,system=0;};
    void* manager=reinterpret_cast<void*(*)()>(ue::Base()+0x12e0e00)();void* platform=manager?reinterpret_cast<void*(*)(void*,const wchar_t*)>(ue::Base()+0x12df700)(manager,L"PakFile"):nullptr;
    if(!platform){note="PakFile platform unavailable";return false;}const std::wstring filename=pak.wstring();Args args;args.filename=filename.c_str();Status status;
    mounted=reinterpret_cast<bool(*)(void*,const Args*,Status*,void*)>(ue::Base()+0x22f8990)(platform,&args,&status,nullptr);
    note=mounted?"Mounted":"Mount failed";Log(note+" io="+std::to_string(status.code));return mounted;}
template<class Release> void Drop(Entry& e,Release release){
    for(auto& c:e.colliders){if(Same(c.object,c.name))Call(c.object,"K2_DestroyComponent").Set("Object",c.object).Invoke();if(c.object)ue::ReleaseReference(c.object);c={};}
    if(Same(e.display,e.display_name)){release(e.display);Call(e.display,"K2_DestroyComponent").Set("Object",e.display).Invoke();}
    if(e.display)ue::ReleaseReference(e.display);
    if(Same(e.source,e.source_name)){if(FlagValue(e.source,e.skin))HideSkin(e.source,e.skin,e.hidden_skin);if(e.tick_at>=0&&ue::At<uint8_t>(e.source,e.tick_at)==0)ue::At<uint8_t>(e.source,e.tick_at)=e.tick;}
    if(e.source)ue::ReleaseReference(e.source);e={};Log("display removed; source restored");}
template<class Release> void Begin(bool enabled,bool transitioning,Release release){for(auto it=entries.begin();it!=entries.end();){
    if(!enabled||transitioning||!Same(it->source,it->source_name)||Property(it->source,"SkinnedAsset")!=it->mesh||!Same(it->mesh,it->mesh_name)||!Same(it->display,it->display_name)){Drop(*it,release);it=entries.erase(it);}else{it->wanted=false;++it;}}
    if(!enabled)note="Disabled";}
inline Object* Ensure(Object* source){if(!ue::Alive(source))return source;
    for(auto& e:entries)if(e.source==source){e.wanted=true;if(Visible(e.display)!=Visible(source))Show(e.display,Visible(source));return e.display;}
    if(entries.size()>=4||!Mount())return source;Object* original=Property(source,"SkinnedAsset");if(!ue::Alive(original))return source;
    const std::string name=Text(original),source_package=Text(ue::OuterOf(original));const char* asset_path=nullptr;for(const auto& item:BoneSupplementData::Meshes)if(!_stricmp(item.source,source_package.c_str())){asset_path=item.asset;break;}
    // Missing assets are cached per mesh, avoiding repeated blocking loads every frame.
    static std::vector<std::pair<Object*,Name>> missing;for(const auto& m:missing)if(m.first==original&&m.second==ue::NameOf(original))return source;
    Object* mesh=asset_path?Load(asset_path):nullptr;if(!ue::Alive(mesh)){missing.push_back({original,ue::NameOf(original)});note="No supplemental mesh for "+name;Log(note);return source;}
    const ue::Array old_bones=ue::At<ue::Array>(original,0x300),new_bones=ue::At<ue::Array>(mesh,0x300);
    if(old_bones.num<1||old_bones.num>2044||new_bones.num!=old_bones.num+4){missing.push_back({original,ue::NameOf(original)});note="Supplement bone count mismatch "+name;Log(note);return source;}
    Call owner(source,"GetOwner");if(!owner.Invoke())return source;Object* actor=owner.Get<Object*>("ReturnValue");
    Object* cls=ue::FindObject("/Script/Engine.SkeletalMeshComponent");Object* copy=Load("/HuaShira/Characters/Hero/Hero_Hun/Hun_CopyPose.Hun_CopyPose_C",true);if(!cls||!copy)return source;
    Entry e;e.source=source;e.source_name=ue::NameOf(source);e.mesh=original;e.mesh_name=ue::NameOf(original);e.visible=Visible(source);e.wanted=true;e.skin=Flag(source,"bHideSkin");if(e.skin.at<0)return source;e.hidden_skin=FlagValue(source,e.skin);
    if(!ue::AddReference(source))return source;
    Call add(actor,"AddComponentByClass");if(!add.Set("Class",cls).Set("bManualAttachment",true).Set("RelativeTransform",Transform{}).Set("bDeferredFinish",true).Invoke()){ue::ReleaseReference(source);return source;}
    e.display=add.Get<Object*>("ReturnValue");if(!ue::Alive(e.display)){ue::ReleaseReference(source);return source;}if(!ue::AddReference(e.display)){Call(e.display,"K2_DestroyComponent").Set("Object",e.display).Invoke();ue::ReleaseReference(source);return source;}e.display_name=ue::NameOf(e.display);Object* d=e.display;
    const auto fail=[&](){Drop(e,[](Object*){});note="Display initialization failed "+name;Log(note);return source;};
    if(!Show(d,false)||!Call(d,"SetSkeletalMeshAsset").Set("NewMesh",mesh).Invoke()||!Call(d,"SetDisablePostProcessBlueprint").Set("bInDisablePostProcess",true).Invoke()||
       !Call(d,"SetAnimInstanceClass").Set("NewClass",copy).Invoke()||!Call(d,"SetCollisionEnabled").Set("NewType",uint8_t{0}).Invoke()||
       !Call(d,"SetGenerateOverlapEvents").Set("bInGenerateOverlapEvents",false).Invoke()||
       !Call(d,"K2_AttachToComponent").Set("Parent",source).Set("SocketName",Name{}).Set("LocationRule",uint8_t{0}).Set("RotationRule",uint8_t{0}).Set("ScaleRule",uint8_t{0}).Set("bWeldSimulatedBodies",false).Invoke()||
       !Call(d,"AddTickPrerequisiteComponent").Set("PrerequisiteComponent",source).Invoke())return fail();
    Call count(source,"GetNumMaterials");if(!count.Invoke())return fail();const int materials=count.Get<int>("ReturnValue");if(materials<0||materials>64)return fail();
    for(int i=0;i<materials;++i){Call get(source,"GetMaterial");if(!get.Set("ElementIndex",i).Invoke()||!Call(d,"SetMaterial").Set("ElementIndex",i).Set("Material",get.Get<Object*>("ReturnValue")).Invoke())return fail();}
    const int tick=ue::Offset(ue::ClassOf(d),"VisibilityBasedAnimTickOption");if(tick<0)return fail();ue::At<uint8_t>(d,tick)=0;
    if(!Call(actor,"FinishAddComponent").Set("Component",d).Set("bManualAttachment",true).Set("RelativeTransform",Transform{}).Invoke())return fail();
    e.tick_at=ue::Offset(ue::ClassOf(source),"VisibilityBasedAnimTickOption");if(e.tick_at<0)return fail();e.tick=ue::At<uint8_t>(source,e.tick_at);ue::At<uint8_t>(source,e.tick_at)=0;
    // The new bones have no bodies in the source PhysicsAsset. Give each its own
    // component-owned box; never mutate that shared asset or duplicate its bodies.
    const BoneSupplementData::CollisionRegion* region=nullptr;const std::string package=Text(ue::OuterOf(mesh));
    for(const auto& r:BoneSupplementData::CollisionRegions)if(package==r.asset){region=&r;break;}
    Object* box_class=ue::FindObject("/Script/Engine.BoxComponent");if(!region||!box_class)return fail();
    const ue::Array ref=ue::At<ue::Array>(mesh,0x310);if(ref.num!=new_bones.num||!ref.data||!new_bones.data)return fail();
    std::vector<Transform> rest(static_cast<size_t>(ref.num));
    for(int i=0;i<ref.num;++i){const int parent=ue::At<int32_t>(static_cast<Object*>(new_bones.data)+i*12,8);const Transform& local=static_cast<const Transform*>(ref.data)[i];
        if(parent==-1)rest[static_cast<size_t>(i)]=local;else if(parent<0||parent>=i||!NpcGripMath::ComposeMount(local,rest[static_cast<size_t>(parent)],rest[static_cast<size_t>(i)]))return fail();}
    // Appearance copies disable their own collision; use their owner's actual
    // character mesh policy, including temporary gameplay collision disabling.
    e.collision_source=Property(actor,"Mesh");if(!ue::Alive(e.collision_source))e.collision_source=source;e.collision_source_name=ue::NameOf(e.collision_source);
    Call object_type(e.collision_source,"GetCollisionObjectType");if(!object_type.Invoke())return fail();
    constexpr const char* collision_bones[]={"pbz_breast_l","pbz_breast_r","pbz_butt_l","pbz_butt_r"};
    for(size_t i=0;i<e.colliders.size();++i){auto& c=e.colliders[i];
        // Runtime skeletons append virtual bones after the raw bones. Resolve by
        // name; old final-bone count is not the insertion point of the new bones.
        for(int b=0;b<new_bones.num;++b)if(ue::NameIs(ue::At<Name>(static_cast<Object*>(new_bones.data)+b*12,0),collision_bones[i])){c.bone=b;break;}
        if(c.bone<0)return fail();NpcGripMath::Inverse(rest[static_cast<size_t>(c.bone)].rotation,c.offset.rotation);
        Call create(actor,"AddComponentByClass");if(!create.Set("Class",box_class).Set("bManualAttachment",true).Set("RelativeTransform",Transform{}).Set("bDeferredFinish",true).Invoke())return fail();
        c.object=create.Get<Object*>("ReturnValue");if(!ue::Alive(c.object))return fail();c.name=ue::NameOf(c.object);if(!ue::AddReference(c.object)){Call(c.object,"K2_DestroyComponent").Set("Object",c.object).Invoke();c.object=nullptr;return fail();}
        std::array<double,3> extent{};for(size_t axis=0;axis<3;++axis)extent[axis]=region->radii[i][axis];
        if(!Call(c.object,"SetBoxExtent").Set("InBoxExtent",extent).Set("bUpdateOverlaps",false).Invoke()||
           !Call(c.object,"SetCollisionEnabled").Set("NewType",uint8_t{0}).Invoke()||
           !Call(c.object,"SetCollisionObjectType").Set("Channel",object_type.Get<uint8_t>("ReturnValue")).Invoke()||
           !Call(c.object,"SetGenerateOverlapEvents").Set("bInGenerateOverlapEvents",false).Invoke())return fail();
        for(uint8_t channel=0;channel<32;++channel){Call response(e.collision_source,"GetCollisionResponseToChannel");if(!response.Set("Channel",channel).Invoke()||!Call(c.object,"SetCollisionResponseToChannel").Set("Channel",channel).Set("NewResponse",response.Get<uint8_t>("ReturnValue")).Invoke())return fail();}
        const Name socket=ue::At<Name>(static_cast<Object*>(new_bones.data)+c.bone*12,0);
        if(!Call(c.object,"K2_AttachToComponent").Set("Parent",d).Set("SocketName",socket).Set("LocationRule",uint8_t{0}).Set("RotationRule",uint8_t{0}).Set("ScaleRule",uint8_t{0}).Set("bWeldSimulatedBodies",false).Invoke()||
           !Call(actor,"FinishAddComponent").Set("Component",c.object).Set("bManualAttachment",true).Set("RelativeTransform",c.offset).Invoke())return fail();
    }
    if(release_source)release_source(source);if(!HideSkin(source,e.skin,true)||!Show(d,e.visible))return fail();entries.push_back(e);note="Active "+name;Log(note+" original="+std::to_string(old_bones.num)+" supplemented="+std::to_string(new_bones.num)+" colliders=4");return d;
}
inline void SyncCollisions(Object* display,const ue::Array& pose){
    for(auto& e:entries){if(e.display!=display||!Same(e.source,e.source_name)||!Same(e.collision_source,e.collision_source_name))continue;
        Call get(display,"K2_GetComponentToWorld"),enabled(e.collision_source,"GetCollisionEnabled");if(!get.Invoke()||!enabled.Invoke())return;
        const Transform world=get.Get<Transform>("ReturnValue");const uint8_t mode=enabled.Get<uint8_t>("ReturnValue");
        for(auto& c:e.colliders){if(!Same(c.object,c.name)||c.bone<0||c.bone>=pose.num)continue;Transform component,target;
            if(!NpcGripMath::ComposeMount(c.offset,static_cast<const Transform*>(pose.data)[c.bone],component)||!NpcGripMath::ComposeMount(component,world,target))continue;
            if(!Call(c.object,"K2_SetWorldTransform").Set("NewTransform",target).Set("bSweep",false).Set("bTeleport",true).Invoke())continue;
            Call current(c.object,"GetCollisionEnabled");if(current.Invoke()&&current.Get<uint8_t>("ReturnValue")!=mode)Call(c.object,"SetCollisionEnabled").Set("NewType",mode).Invoke();
        }return;
    }
}
inline void ProbeCollisions(){
    for(const auto& e:entries)for(const auto& c:e.colliders){if(!Same(c.object,c.name))continue;
        Call get(c.object,"K2_GetComponentToWorld"),extent(c.object,"GetScaledBoxExtent"),enabled(c.object,"GetCollisionEnabled");
        if(!get.Invoke()||!extent.Invoke()||!enabled.Invoke())continue;
        const Transform world=get.Get<Transform>("ReturnValue");const auto half=extent.Get<std::array<double,3>>("ReturnValue");
        double error=0;bool valid=enabled.Get<uint8_t>("ReturnValue")!=0;
        for(size_t axis=0;axis<3;++axis){double local[3]{},delta[3]{};local[axis]=half[axis]*1.5;NpcGripMath::Rotate(world.rotation,local,delta);
            std::array<double,3> point{};for(size_t k=0;k<3;++k)point[k]=world.translation[k]+delta[k];
            Call query(c.object,"GetClosestPointOnCollision");if(!query.Set("Point",point).Set("BoneName",Name{}).Invoke()){valid=false;continue;}
            const float distance=query.Get<float>("ReturnValue");if(distance<0)valid=false;else error=(std::max)(error,std::abs(static_cast<double>(distance)-half[axis]*.5));
        }
        Log("collision probe bone="+std::to_string(c.bone)+" mode="+std::to_string(enabled.Get<uint8_t>("ReturnValue"))+" valid="+std::to_string(valid?1:0)+" error_cm="+std::to_string(error));
    }
}
template<class Release> void End(Release release){for(auto it=entries.begin();it!=entries.end();)if(!it->wanted){Drop(*it,release);it=entries.erase(it);}else ++it;}
inline Object* Display(Object* source){for(const auto& e:entries)if(e.source==source&&Same(e.display,e.display_name))return e.display;return source;}
}
