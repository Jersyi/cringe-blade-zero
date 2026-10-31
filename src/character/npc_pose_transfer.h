#pragma once
#include "npc_grip_math.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace NpcPoseTransfer {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4324)
#endif
struct alignas(16) Transform { double rotation[4]{0,0,0,1},translation[4]{},scale[4]{1,1,1,0}; };
static_assert(sizeof(Transform)==96);
struct Bone { uint64_t name=0;int32_t parent=-1;Transform ref; };
struct Plan {
    std::vector<Bone> source,target;
    std::vector<int32_t> target_to_source,source_to_target;
    std::vector<std::array<double,4>> q0,q1;
    std::vector<Transform> source_ref_cs,target_ref_cs;
    size_t mapped=0;
    size_t target_raw=0;
};
struct Cache {
    std::vector<Transform> snapshot,target_cs,synthetic_source,source_local;
    uint64_t builds=0,hits=0;bool ready=false;
};
inline bool ReferenceCS(const std::vector<Bone>& bones,std::vector<Transform>& out,std::string& why) {
    if(bones.empty()||bones.size()>2048){why="reference-count";return false;}out.resize(bones.size());
    for(size_t i=0;i<bones.size();++i){const Bone& b=bones[i];if(!NpcGripMath::Valid(b.ref)||b.parent>=static_cast<int32_t>(i)||(i==0?b.parent!=-1:b.parent<0)){why="reference-tree";return false;}
        if(b.parent<0)out[i]=b.ref;else if(!NpcGripMath::Compose(b.ref,out[static_cast<size_t>(b.parent)],out[i])){why="reference-compose";return false;}}
    return true;
}
inline bool Build(Plan& plan,std::vector<Bone> source,std::vector<Bone> target,const std::vector<std::array<double,4>>& skeleton_q0,std::string& why,size_t target_raw=0) {
    Plan next;next.source=std::move(source);next.target=std::move(target);
    if(!skeleton_q0.empty()&&skeleton_q0.size()!=next.target.size()){why="skeleton-q0-count";return false;}
    if(!ReferenceCS(next.source,next.source_ref_cs,why)||!ReferenceCS(next.target,next.target_ref_cs,why))return false;
    std::map<uint64_t,int32_t> names;for(size_t i=0;i<next.source.size();++i)if(!names.emplace(next.source[i].name,static_cast<int32_t>(i)).second){why="source-duplicate-name";return false;}
    std::map<uint64_t,int32_t> target_names;for(size_t i=0;i<next.target.size();++i)if(!target_names.emplace(next.target[i].name,static_cast<int32_t>(i)).second){why="target-duplicate-name";return false;}
    next.target_raw=target_raw?target_raw:next.target.size();if(next.target_raw>next.target.size()){why="target-raw-count";return false;}
    next.target_to_source.assign(next.target.size(),-1);next.source_to_target.assign(next.source.size(),-1);
    next.q0.resize(next.target.size());next.q1.resize(next.target.size());
    for(size_t t=0;t<next.target_raw;++t){auto it=names.find(next.target[t].name);if(it==names.end())continue;const int32_t s=it->second,sp=next.source[static_cast<size_t>(s)].parent,tp=next.target[t].parent;
        if((sp<0)!=(tp<0)||(sp>=0&&next.source[static_cast<size_t>(sp)].name!=next.target[static_cast<size_t>(tp)].name)){why="shared-parent-mismatch";return false;}
        next.target_to_source[t]=s;next.source_to_target[static_cast<size_t>(s)]=static_cast<int32_t>(t);++next.mapped;
        auto& q0=next.q0[t];if(skeleton_q0.size()==next.target.size())q0=skeleton_q0[t];else if(tp<0)q0={0,0,0,1};else{double qi[4];NpcGripMath::Inverse(next.target_ref_cs[static_cast<size_t>(tp)].rotation,qi);NpcGripMath::Mul(qi,next.source_ref_cs[static_cast<size_t>(sp)].rotation,q0.data());}
        double qlength=0;for(double v:q0){if(!std::isfinite(v)){why="skeleton-q0-invalid";return false;}qlength+=v*v;}if(std::abs(qlength-1)>0.001){why="skeleton-q0-nonunit";return false;}
        double si[4],qi[4],tmp[4];NpcGripMath::Inverse(next.source[static_cast<size_t>(s)].ref.rotation,si);NpcGripMath::Inverse(q0.data(),qi);NpcGripMath::Mul(si,qi,tmp);NpcGripMath::Mul(tmp,next.target[t].ref.rotation,next.q1[t].data());
    }
    if(!next.mapped||next.target_to_source[0]!=0){why="root-mapping";return false;}plan=std::move(next);return true;
}
inline void Prepare(const Plan& plan,Cache& cache){cache.snapshot.resize(plan.source.size());cache.synthetic_source.resize(plan.source.size());cache.source_local.resize(plan.source.size());cache.target_cs.resize(plan.target.size());cache.ready=false;cache.builds=cache.hits=0;}
inline bool Evaluate(const Plan& plan,const Transform* source,size_t count,Cache& cache,const char*& why) {
    if(!source||count!=plan.source.size()||cache.snapshot.size()!=count||cache.target_cs.size()!=plan.target.size()){why="pose-count/epoch";return false;}
    if(cache.ready&&!std::memcmp(source,cache.snapshot.data(),count*sizeof(Transform))){++cache.hits;return true;}
    cache.ready=false;
    // All storage is prepared before the session. This path allocates nothing
    // and never edits source or an engine pose array.
    for(size_t s=0;s<count;++s){if(plan.source_to_target[s]<0)continue;const int32_t p=plan.source[s].parent;if(!NpcGripMath::FiniteMount(source[s])){why="source-pose-invalid";return false;}for(size_t axis=0;axis<3;++axis)if(source[s].scale[axis]<0){why="shared-negative-scale-native-matrix-required";return false;}
        if(p<0)cache.source_local[s]=source[s];else if(!NpcGripMath::RelativeNativeSafe(source[s],source[static_cast<size_t>(p)],cache.source_local[s])){why="source-local-invalid";return false;}}
    for(size_t t=0;t<plan.target.size();++t){Transform local=plan.target[t].ref;const int32_t s=plan.target_to_source[t];
        if(s>=0){const Transform& input=cache.source_local[static_cast<size_t>(s)],&ref=plan.source[static_cast<size_t>(s)].ref;double tmp[4],delta[3],rotated[3];
            NpcGripMath::Mul(plan.q0[t].data(),input.rotation,tmp);NpcGripMath::Mul(tmp,plan.q1[t].data(),local.rotation);
            for(size_t i=0;i<3;++i)delta[i]=input.translation[i]-ref.translation[i];NpcGripMath::Rotate(plan.q0[t].data(),delta,rotated);
            for(size_t i=0;i<3;++i){local.translation[i]+=rotated[i];local.scale[i]=input.scale[i]*local.scale[i]/ref.scale[i];}}
        const int32_t p=plan.target[t].parent;if(p<0)cache.target_cs[t]=local;else if(!NpcGripMath::ComposeMount(local,cache.target_cs[static_cast<size_t>(p)],cache.target_cs[t])){why="target-compose-invalid";return false;}}
    std::memcpy(cache.synthetic_source.data(),source,count*sizeof(Transform));
    for(size_t s=0;s<count;++s)if(plan.source_to_target[s]>=0)cache.synthetic_source[s]=cache.target_cs[static_cast<size_t>(plan.source_to_target[s])];
    std::memcpy(cache.snapshot.data(),source,count*sizeof(Transform));cache.ready=true;++cache.builds;why="ok";return true;
}
#ifdef _MSC_VER
#pragma warning(pop)
#endif
}
