#pragma once
#include "ue_runtime.h"
#include "game_build.h"

// Game thread only. Resubmit the final bone pose through Unreal's physics API:
// it updates each kinematic body's position, rotation and shape scale under the
// native scene write lock. Do not change shared PhysicsAssets or teleport ragdolls.
namespace BoneCollision {
inline bool Supported(){
    static constexpr unsigned char bytes[]={0x40,0x55,0x53,0x56,0x57,0x41,0x57,0x48,0x8d,0x6c,0x24,0xd1,0x48,0x81,0xec,0xf0,0,0,0,0xf6,0x81,0x98,0,0,0,0x08}; // exe-bytes: 0x436efc0
    return GameBuild::CodeMatches(ue::Base()+0x436efc0,bytes,sizeof(bytes));
}
inline void Sync(ue::Object* component,const ue::Array& pose){
    // ETeleportType::None, no per-poly skinning, DisallowDeferral. This is the
    // same function used by SkeletalMeshComponent::OnUpdateTransform (slot 174).
    // Immediate submission prevents a deferred update reading a different buffer.
    if(Supported())reinterpret_cast<void(*)(ue::Object*,const ue::Array&,uint8_t,bool,int)>(ue::Base()+0x436efc0)(component,pose,0,false,1);
}
}
