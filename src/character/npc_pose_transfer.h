#pragma once
#include "npc_grip_math.h"
#include <algorithm>
#include <array>
#include <cmath>
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
// Two-handed grips: the weapon adapter (NpcGripMath::MapSourceMountPose) turns
// the weapon about the right palm by R = target hand_r * basis_r * inverse(source
// hand_r), because NPC hands are shaped and posed differently. While the source
// palms are close, the left palm keeps the source offset from the right palm
// turned by the same R and scaled by Plan::weapon_scale like the weapon, and the
// left hand turns by R as well, so it sits on the weapon where the source hand
// was. Solved as a two-bone arm IK.
// Grip gate (source_weapon_r >= 0): close palms alone do not mean a two-handed grip (a free
// left hand hangs 50-100 cm from a one-handed sword). The source's left fist must hold the
// right hand's weapon: its grip point (hand_l * grip_l, the rest weapon_l offset) within
// grip_full_cm..grip_none_cm of the weapon_r handle axis (local Z), the two handle axes
// parallel within axis_full_deg..axis_none_deg.
struct HandContact {
    int32_t source_hand_l=-1,source_hand_r=-1,source_palm_l=-1,source_palm_r=-1,source_weapon_r=-1;
    int32_t upper=-1,lower=-1,hand=-1,hand_r=-1,palm_l=-1,palm_r=-1;
    double basis_l[4]{0,0,0,1},basis_r[4]{0,0,0,1};
    Transform grip_l;
    double full_cm=75,none_cm=105,grip_full_cm=6,grip_none_cm=12,axis_full_deg=30,axis_none_deg=50;
    bool enabled=false;
};
struct Plan {
    std::vector<Bone> source,target;
    std::vector<int32_t> target_to_source,source_to_target;
    std::vector<std::array<double,4>> q0,q1;
    std::vector<Transform> source_ref_cs,target_ref_cs;
    size_t mapped=0,parent_mismatch=0;
    size_t target_raw=0;
    HandContact contact;
    // Retarget pose: a target bone can start from the source's rest pose instead of its own.
    // rest_turn[t] = inverse(source ref CS) * A * target ref CS, so the bone's component-space
    // rotation is source CS * rest_turn. rest_mode 1 = mapped and turned (own A or a turned
    // ancestor's), 2 = unmapped under a turned bone (follows its parent).
    std::vector<std::array<double,4>> rest_align,rest_turn;std::vector<uint8_t> rest_set,rest_mode;
    std::vector<Transform> target_rest_cs; // the target's pose while the source holds its rest pose
    double weapon_scale=1; // target / source body size (WeaponScale), applied to weapons and the grip offset
    std::vector<uint8_t> authored_follow; // selected IK targets retain authored travel during their own montage
    std::vector<int32_t> follow;std::vector<Transform> follow_rel; // SetFollowers; empty when none
    std::vector<uint8_t> foreign_keep; // per target bone: keeps its translation under a foreign montage (root, pelvis); empty = none
    // per target bone: its source bone's translation retarget mode on the source skeleton (kScaled,
    // kAnimation, kSkeleton); empty = all kScaled. Used to undo the remap of the target's own montages.
    std::vector<uint8_t> source_mode;
    // per target bone: the target Skeleton's own translation retarget mode and reference translation
    // length (the length the engine divides by when it plays that Skeleton's montages on the source).
    // Empty, or a bone without them: kScaled, as long as the display rest (built like the display mesh).
    std::vector<uint8_t> own_mode; std::vector<double> own_length;
    // Pose puppet (SetPuppet): the target mesh playing the target's own animation. puppet_always: raw
    // target bones that always take its pose (a mount, the rider's pelvis and legs, bones the source has
    // none of); every other raw bone but the root takes it by PuppetPose::body and, when puppet_own, by
    // the own-montage weight. extra_source: per target bone, a source slot no target bone maps, carrying
    // that bone's pose through the source-shaped pose (synthetic_source), else -1; extra_order lists those
    // bones by slot. All empty = no puppet. extra_mesh: the CopyPose copies component space (a slot then
    // holds the bone's component-space transform, not its local on the slot's source parent).
    std::vector<uint8_t> puppet_always; std::vector<int32_t> extra_source,extra_order; bool puppet_own=true,extra_mesh=false;
};
enum : uint8_t { kScaled = 0, kAnimation = 1, kSkeleton = 2 };
// The puppet's current component-space pose (target bone order). serial changes with the animation it
// plays; the target crossfades from its last puppet pose over kPuppetBlendMs (now_ms is that clock).
// body: how much of every raw bone but the root takes the puppet's pose (0..1; the target's own locomotion).
struct PuppetPose { const Transform* cs=nullptr;size_t count=0;uint32_t serial=0;uint64_t now_ms=0;double body=0; };
constexpr uint64_t kPuppetBlendMs=200;
struct Cache {
    std::vector<Transform> snapshot,target_cs,synthetic_source,source_local,target_local;std::vector<uint8_t> moved;
    uint64_t builds=0,hits=0,contacts=0,grip_rejected=0;bool ready=false;double contact_weight=0,contact_error_max=0,foreign=0;bool own=false;
    // Puppet: the last puppet locals, the locals a switch fades from, the pose last built from, and per
    // bone the turn the puppet added above the source-driven pose (rotates the retarget-pose turns).
    std::vector<Transform> puppet_last,puppet_from,puppet_snapshot;std::vector<std::array<double,4>> shift;
    uint32_t puppet_serial=0;uint64_t puppet_switch_ms=0,puppet_builds=0;bool puppet_seen=false,puppet_used=false,puppet_faded=false;
    double puppet_body=0; // PuppetPose::body of the pose last built (0 without a puppet)
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
        // A same-name bone under a different parent (a boss's eye or skirt bone) keeps the NPC reference pose.
        if((sp<0)!=(tp<0)||(sp>=0&&next.source[static_cast<size_t>(sp)].name!=next.target[static_cast<size_t>(tp)].name)){if(t==0){why="shared-parent-mismatch";return false;}++next.parent_mismatch;continue;}
        next.target_to_source[t]=s;next.source_to_target[static_cast<size_t>(s)]=static_cast<int32_t>(t);++next.mapped;
        auto& q0=next.q0[t];if(skeleton_q0.size()==next.target.size())q0=skeleton_q0[t];else if(tp<0)q0={0,0,0,1};else{double qi[4];NpcGripMath::Inverse(next.target_ref_cs[static_cast<size_t>(tp)].rotation,qi);NpcGripMath::Mul(qi,next.source_ref_cs[static_cast<size_t>(sp)].rotation,q0.data());}
        double qlength=0;for(double v:q0){if(!std::isfinite(v)){why="skeleton-q0-invalid";return false;}qlength+=v*v;}if(std::abs(qlength-1)>0.001){why="skeleton-q0-nonunit";return false;}
        double si[4],qi[4],tmp[4];NpcGripMath::Inverse(next.source[static_cast<size_t>(s)].ref.rotation,si);NpcGripMath::Inverse(q0.data(),qi);NpcGripMath::Mul(si,qi,tmp);NpcGripMath::Mul(tmp,next.target[t].ref.rotation,next.q1[t].data());
    }
    if(!next.mapped||next.target_to_source[0]!=0){why="root-mapping";return false;}next.target_rest_cs=next.target_ref_cs;plan=std::move(next);return true;
}
// Index of the bone whose raw FName is `name`, else -1 (FNames compare case-insensitively by index).
inline int32_t BoneNamed(const std::vector<Bone>& bones,uint64_t name){for(size_t i=0;i<bones.size();++i)if(bones[i].name==name)return static_cast<int32_t>(i);return -1;}
inline void Prepare(const Plan& plan,Cache& cache){cache.snapshot.resize(plan.source.size());cache.synthetic_source.resize(plan.source.size());cache.source_local.resize(plan.source.size());cache.target_cs.resize(plan.target.size());cache.target_local.resize(plan.target.size());cache.moved.assign(plan.target.size(),0);cache.ready=false;cache.builds=cache.hits=cache.contacts=cache.grip_rejected=0;cache.contact_weight=cache.contact_error_max=cache.foreign=0;cache.own=false;
    const size_t puppet=plan.puppet_always.size()==plan.target.size()?plan.target.size():0;cache.puppet_last.assign(puppet,Transform{});cache.puppet_from.assign(puppet,Transform{});cache.puppet_snapshot.assign(puppet,Transform{});
    cache.shift.assign(puppet,{0,0,0,1});cache.puppet_serial=0;cache.puppet_switch_ms=cache.puppet_builds=0;cache.puppet_seen=cache.puppet_used=cache.puppet_faded=false;cache.puppet_body=0;}
// Pose puppet. always: per target bone, 1 = always takes the puppet's pose (raw non-root bones only).
// Unmapped raw bones get the source's unmapped slots (never the root's), always-bones first, in bone
// order, while slots last; assigned / wanted report how many. Call after Build, before Prepare.
inline bool SetPuppet(Plan& plan,const std::vector<uint8_t>& always,size_t& assigned,size_t& wanted) {
    const size_t n=plan.target.size();assigned=wanted=0;plan.puppet_always.clear();plan.extra_source.clear();plan.extra_order.clear();if(always.size()!=n||!n)return false;
    plan.puppet_always.assign(n,0);for(size_t t=1;t<plan.target_raw;++t)plan.puppet_always[t]=always[t]?1:0;
    std::vector<int32_t> spare;for(size_t s=1;s<plan.source.size();++s)if(plan.source_to_target[s]<0&&plan.source[s].parent>=0)spare.push_back(static_cast<int32_t>(s));
    plan.extra_source.assign(n,-1);size_t next=0;
    for(int pass=0;pass<2;++pass)for(size_t t=1;t<plan.target_raw;++t){if(plan.target_to_source[t]>=0||(pass==0)!=(plan.puppet_always[t]!=0))continue;++wanted;
        if(next<spare.size()){plan.extra_source[t]=spare[next++];plan.extra_order.push_back(static_cast<int32_t>(t));++assigned;}}
    // A slot's CopyPose local is relative to its source parent: parents are written first.
    std::sort(plan.extra_order.begin(),plan.extra_order.end(),[&](int32_t a,int32_t b){return plan.extra_source[static_cast<size_t>(a)]<plan.extra_source[static_cast<size_t>(b)];});
    return true;
}
// Indices are plan bone indices; the target chain must be upper -> lower -> hand.
inline bool SetHandContact(Plan& plan,HandContact request) {
    plan.contact=HandContact{};const int32_t sources=static_cast<int32_t>(plan.source.size()),targets=static_cast<int32_t>(plan.target.size());
    const auto in=[](int32_t v,int32_t n){return v>=0&&v<n;};
    const auto unit=[](const double* q){double n=0;for(size_t i=0;i<4;++i){if(!std::isfinite(q[i]))return false;n+=q[i]*q[i];}return std::abs(n-1)<0.001;};
    const HandContact& k=request;
    if(!in(k.source_hand_l,sources)||!in(k.source_hand_r,sources)||!in(k.source_palm_l,sources)||!in(k.source_palm_r,sources)||
        !in(k.upper,targets)||!in(k.lower,targets)||!in(k.hand,targets)||!in(k.hand_r,targets)||!in(k.palm_l,targets)||!in(k.palm_r,targets)||
        !unit(k.basis_l)||!unit(k.basis_r)||!(k.full_cm>0&&k.none_cm>k.full_cm))return false;
    if(k.source_weapon_r>=0&&(!in(k.source_weapon_r,sources)||!NpcGripMath::FiniteMount(k.grip_l)||!unit(k.grip_l.rotation)||
        !(k.grip_full_cm>0&&k.grip_none_cm>k.grip_full_cm)||!(k.axis_full_deg>=0&&k.axis_none_deg>k.axis_full_deg&&k.axis_none_deg<=90)))return false;
    if(plan.target[static_cast<size_t>(k.lower)].parent!=k.upper||plan.target[static_cast<size_t>(k.hand)].parent!=k.lower)return false;
    for(int32_t b=k.palm_l;b>=0;b=plan.target[static_cast<size_t>(b)].parent)if(b==k.hand){plan.contact=request;plan.contact.enabled=true;return true;}
    return false;
}
// Body size for weapons: the geometric mean of three target / source ratios measured on the
// reference poses, stature (head height), reach (upper arm + forearm) and fist (square root of
// hand length times knuckle width), each averaged over both sides. One odd proportion does not
// decide it alone (Scavenger's arms are 4.9x the hero's, its head 2.7x). A ratio whose bones are
// missing on either skeleton is left out; at least two must remain. Bones are plan indices.
struct SizeBones { int32_t head=-1;int32_t upper[2]{-1,-1},lower[2]{-1,-1},hand[2]{-1,-1},middle[2]{-1,-1},index[2]{-1,-1},pinky[2]{-1,-1}; };
struct BodyScale { double source[3]{},target[3]{},ratio[3]{},raw=1,scale=1;int32_t used=0; };
inline void MeasureBody(const std::vector<Transform>& cs,const SizeBones& b,double out[3]) {
    const auto in=[&](int32_t v){return v>=0&&static_cast<size_t>(v)<cs.size();};
    const auto span=[&](int32_t x,int32_t y){double d=0;for(size_t i=0;i<3;++i){const double v=cs[static_cast<size_t>(y)].translation[i]-cs[static_cast<size_t>(x)].translation[i];d+=v*v;}return std::sqrt(d);};
    out[0]=out[1]=out[2]=0;if(in(b.head)&&cs[static_cast<size_t>(b.head)].translation[2]>20)out[0]=cs[static_cast<size_t>(b.head)].translation[2];
    double reach=0,fist=0;int32_t arms=0,fists=0;
    for(size_t s=0;s<2;++s){if(in(b.upper[s])&&in(b.lower[s])&&in(b.hand[s])){reach+=span(b.upper[s],b.lower[s])+span(b.lower[s],b.hand[s]);++arms;}
        if(in(b.hand[s])&&in(b.middle[s])&&in(b.index[s])&&in(b.pinky[s])){fist+=std::sqrt(span(b.hand[s],b.middle[s])*span(b.index[s],b.pinky[s]));++fists;}}
    if(arms)out[1]=reach/arms;if(fists)out[2]=fist/fists;
}
// IK copies of hand bones (Hun's ik_weapon_r / ik_weapon_r_child / ik_hand_r / ik_hand_gun hang off
// the root but rest exactly on weapon_r / hand_r): a weapon mounted on one must follow the hand, not
// the root. For each bone outside the hands' chains, the hand-chain bone resting within tolerance_cm
// of it (nearest), else -1. Works on either skeleton: bones / ref_cs / hands of the same side.
inline std::vector<int32_t> HandProxies(const std::vector<Bone>& bones,const std::vector<Transform>& ref_cs,const std::vector<int32_t>& hands,double tolerance_cm) {
    const size_t n=bones.size();std::vector<uint8_t> chain(n,0);std::vector<int32_t> proxy(n,-1);if(ref_cs.size()!=n)return proxy;
    for(size_t s=0;s<n;++s)for(int32_t b=static_cast<int32_t>(s);b>=0;b=bones[static_cast<size_t>(b)].parent)
        if(std::find(hands.begin(),hands.end(),b)!=hands.end()){chain[s]=1;break;}
    for(size_t s=0;s<n;++s){if(chain[s])continue;double best=tolerance_cm;
        for(size_t h=0;h<n;++h){if(!chain[h])continue;double d=0;for(size_t i=0;i<3;++i){const double v=ref_cs[s].translation[i]-ref_cs[h].translation[i];d+=v*v;}
            d=std::sqrt(d);if(d<=best){best=d;proxy[s]=static_cast<int32_t>(h);}}}
    return proxy;
}
// The NPC's own IK copies (ik_weapon_r, ik_hand_r ...) are retargeted from the hero's root-relative
// IK bones, so they drift off the NPC's hands; its post-process AnimBP (weapon / hand IK in Control
// Rig) then pulls the visible hand toward them and off the weapon. follow[t] = the target bone t
// tracks (it keeps its rest offset to it); Evaluate moves t and everything under it last.
inline bool SetFollowers(Plan& plan,const std::vector<int32_t>& follow) {
    const size_t n=plan.target.size();plan.follow.clear();plan.follow_rel.clear();if(follow.size()!=n)return false;
    std::vector<Transform> rel(n);size_t used=0;
    for(size_t t=0;t<n;++t){const int32_t f=follow[t];if(f<0)continue;if(static_cast<size_t>(f)>=n||f==static_cast<int32_t>(t))return false;
        for(int32_t b=f;b>=0;b=plan.target[static_cast<size_t>(b)].parent)if(b==static_cast<int32_t>(t))return false; // never follow a descendant
        if(follow[static_cast<size_t>(f)]>=0)return false; // followers follow FK bones only
        if(!NpcGripMath::RelativeNativeSafe(plan.target_ref_cs[t],plan.target_ref_cs[static_cast<size_t>(f)],rel[t]))return false;++used;}
    if(used){plan.follow=follow;plan.follow_rel=std::move(rel);}return true;
}
inline bool WeaponScale(Plan& plan,const SizeBones& source,const SizeBones& target,double low,double high,BodyScale& out) {
    out=BodyScale{};plan.weapon_scale=1;if(!(low>0)||!(high>=low)||!std::isfinite(high))return false;
    MeasureBody(plan.source_ref_cs,source,out.source);MeasureBody(plan.target_ref_cs,target,out.target);double product=1;
    for(size_t i=0;i<3;++i)if(out.source[i]>1e-3&&out.target[i]>1e-3&&std::isfinite(out.source[i]+out.target[i])){out.ratio[i]=out.target[i]/out.source[i];product*=out.ratio[i];++out.used;}
    if(out.used<2)return false;out.raw=std::pow(product,1.0/out.used);if(!std::isfinite(out.raw))return false;
    out.scale=(std::min)(high,(std::max)(low,out.raw));plan.weapon_scale=out.scale;return true;
}
namespace detail {
inline double Dot(const double* a,const double* b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
// 1 up to full, 0 from none, smoothstep between.
inline double Fade(double x,double full,double none){if(x<=full)return 1;if(x>=none)return 0;const double t=(none-x)/(none-full);return t*t*(3-2*t);}
inline void Slerp(const double* a,const double* b,double t,double* out) {
    double d=a[0]*b[0]+a[1]*b[1]+a[2]*b[2]+a[3]*b[3];const double sign=d<0?-1:1;d=std::abs(d);double wa=1-t,wb=t;
    if(d<0.9995){const double angle=std::acos(d),s=std::sin(angle);wa=std::sin((1-t)*angle)/s;wb=std::sin(t*angle)/s;}
    double n=0;for(size_t i=0;i<4;++i){out[i]=wa*a[i]+sign*wb*b[i];n+=out[i]*out[i];}n=std::sqrt(n);for(size_t i=0;i<4;++i)out[i]/=n;
}
inline bool FromTo(const double* a,const double* b,double* q) {
    const double la=std::sqrt(Dot(a,a)),lb=std::sqrt(Dot(b,b));if(!(la>1e-6)||!(lb>1e-6))return false;
    const double c[3]={a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};double w=la*lb+Dot(a,b);
    if(w<1e-9*la*lb){ // Opposite: any axis orthogonal to a.
        const double axis[3]={std::abs(a[0])<std::abs(a[2])?0.0:-a[1],std::abs(a[0])<std::abs(a[2])?-a[2]:a[0],std::abs(a[0])<std::abs(a[2])?a[1]:0.0};
        const double n=std::sqrt(Dot(axis,axis));if(!(n>1e-9))return false;q[0]=axis[0]/n;q[1]=axis[1]/n;q[2]=axis[2]/n;q[3]=0;return true;}
    double n=std::sqrt(Dot(c,c)+w*w);q[0]=c[0]/n;q[1]=c[1]/n;q[2]=c[2]/n;q[3]=w/n;return std::isfinite(q[0]+q[1]+q[2]+q[3]);
}
// Moves and turns target_cs[hand] so the left palm and hand land at the weighted
// weapon-frame contact; rewrites only upper/lower/hand local rotations.
inline bool SolveHandContact(const Plan& plan,const Transform* source,Cache& cache) {
    const HandContact& k=plan.contact;cache.contact_weight=0;if(!k.enabled)return true;
    const double* sl=source[static_cast<size_t>(k.source_palm_l)].translation,*sr=source[static_cast<size_t>(k.source_palm_r)].translation;
    const double offset[3]={sl[0]-sr[0],sl[1]-sr[1],sl[2]-sr[2]},distance=std::sqrt(Dot(offset,offset));
    if(!std::isfinite(distance)||distance>=k.none_cm)return true;
    double weight=Fade(distance,k.full_cm,k.none_cm);
    // The target's own montage keeps its own hands: its key already holds the weapon; so does its own locomotion.
    if(cache.own)weight*=1-cache.foreign;weight*=1-cache.puppet_body;if(!(weight>0))return true;
    if(k.source_weapon_r>=0){Transform grip;if(!NpcGripMath::ComposeMount(k.grip_l,source[static_cast<size_t>(k.source_hand_l)],grip))return true;
        const Transform& weapon=source[static_cast<size_t>(k.source_weapon_r)];const double up[3]={0,0,1};double axis_r[3],axis_l[3],off[3];
        NpcGripMath::Rotate(weapon.rotation,up,axis_r);NpcGripMath::Rotate(grip.rotation,up,axis_l);
        for(size_t i=0;i<3;++i)off[i]=grip.translation[i]-weapon.translation[i];const double along=Dot(off,axis_r);for(size_t i=0;i<3;++i)off[i]-=along*axis_r[i];
        const double apart=std::sqrt(Dot(off,off)),skew=std::acos((std::min)(1.0,std::abs(Dot(axis_l,axis_r))))*57.29577951308232;
        weight*=Fade(apart,k.grip_full_cm,k.grip_none_cm)*Fade(skew,k.axis_full_deg,k.axis_none_deg);
        if(!(weight>0)){++cache.grip_rejected;return true;}}
    auto& cs=cache.target_cs;const size_t U=static_cast<size_t>(k.upper),L=static_cast<size_t>(k.lower),H=static_cast<size_t>(k.hand);
    // The weapon adapter's turn for the right hand, applied to the left as well.
    double turn[4],tmp[4],inverse[4],turned[3],want_rotation[4],rotation[4];
    NpcGripMath::Inverse(source[static_cast<size_t>(k.source_hand_r)].rotation,inverse);NpcGripMath::Mul(cs[static_cast<size_t>(k.hand_r)].rotation,k.basis_r,tmp);NpcGripMath::Mul(tmp,inverse,turn);
    NpcGripMath::Rotate(turn,offset,turned);for(double& v:turned)v*=plan.weapon_scale;
    NpcGripMath::Inverse(k.basis_l,inverse);NpcGripMath::Mul(turn,source[static_cast<size_t>(k.source_hand_l)].rotation,tmp);NpcGripMath::Mul(tmp,inverse,want_rotation);
    const double* palm=cs[static_cast<size_t>(k.palm_l)].translation,*anchor=cs[static_cast<size_t>(k.palm_r)].translation;
    double goal_palm[3];for(size_t i=0;i<3;++i)goal_palm[i]=palm[i]+(anchor[i]+turned[i]-palm[i])*weight;
    Slerp(cs[H].rotation,want_rotation,weight,rotation);
    // The palm's offset in the hand's rotation frame (scale included) does not
    // change when the hand moves, so the wrist goal follows from the new turn.
    double palm_world[3],palm_hand[3],palm_turned[3],goal[3];for(size_t i=0;i<3;++i)palm_world[i]=palm[i]-cs[H].translation[i];
    NpcGripMath::Inverse(cs[H].rotation,inverse);NpcGripMath::Rotate(inverse,palm_world,palm_hand);NpcGripMath::Rotate(rotation,palm_hand,palm_turned);
    for(size_t i=0;i<3;++i)goal[i]=goal_palm[i]-palm_turned[i];
    const double* A=cs[U].translation,*B=cs[L].translation,*C=cs[H].translation;
    double ab[3],bc[3],at[3];for(size_t i=0;i<3;++i){ab[i]=B[i]-A[i];bc[i]=C[i]-B[i];at[i]=goal[i]-A[i];}
    const double l1=std::sqrt(Dot(ab,ab)),l2=std::sqrt(Dot(bc,bc));double reach=std::sqrt(Dot(at,at));
    if(!(l1>0.5)||!(l2>0.5)||!(reach>1e-3))return true;
    const double dir[3]={at[0]/reach,at[1]/reach,at[2]/reach};
    reach=(std::max)(std::abs(l1-l2)+1e-3,(std::min)(reach,(l1+l2)*0.9995));
    double pole[3];const double along=Dot(ab,dir);for(size_t i=0;i<3;++i)pole[i]=ab[i]-dir[i]*along;double pn=std::sqrt(Dot(pole,pole));
    if(!(pn>1e-4)){double ac[3];for(size_t i=0;i<3;++i)ac[i]=C[i]-A[i];const double d2=Dot(ac,dir);for(size_t i=0;i<3;++i)pole[i]=ac[i]-dir[i]*d2;pn=std::sqrt(Dot(pole,pole));if(!(pn>1e-4))return true;}
    const double a=(l1*l1-l2*l2+reach*reach)/(2*reach),h=std::sqrt((std::max)(0.0,l1*l1-a*a));
    double elbow[3],wrist[3];for(size_t i=0;i<3;++i){elbow[i]=A[i]+dir[i]*a+pole[i]/pn*h;wrist[i]=A[i]+dir[i]*reach;}
    double q1[4],q2[4],to_elbow[3],moved_c[3],ac[3];for(size_t i=0;i<3;++i){to_elbow[i]=elbow[i]-A[i];ac[i]=C[i]-A[i];}
    if(!FromTo(ab,to_elbow,q1))return true;NpcGripMath::Rotate(q1,ac,moved_c);
    double from[3],to[3];for(size_t i=0;i<3;++i){from[i]=A[i]+moved_c[i]-elbow[i];to[i]=wrist[i]-elbow[i];}
    if(!FromTo(from,to,q2))return true;
    Transform upper=cs[U],lower=cs[L],hand=cs[H];
    NpcGripMath::Mul(q1,cs[U].rotation,upper.rotation);NpcGripMath::Mul(q1,cs[L].rotation,tmp);NpcGripMath::Mul(q2,tmp,lower.rotation);
    std::memcpy(hand.rotation,rotation,sizeof(rotation));for(size_t i=0;i<3;++i){lower.translation[i]=elbow[i];hand.translation[i]=wrist[i];}
    const int32_t parent=plan.target[U].parent;Transform local;
    if(parent<0||!NpcGripMath::RelativeNativeSafe(upper,cs[static_cast<size_t>(parent)],local))return true;
    std::memcpy(cache.target_local[U].rotation,local.rotation,sizeof(local.rotation));
    if(!NpcGripMath::RelativeNativeSafe(lower,upper,local))return true;std::memcpy(cache.target_local[L].rotation,local.rotation,sizeof(local.rotation));
    if(!NpcGripMath::RelativeNativeSafe(hand,lower,local))return true;std::memcpy(cache.target_local[H].rotation,local.rotation,sizeof(local.rotation));
    for(size_t t=U;t<plan.target.size();++t){const int32_t p=plan.target[t].parent;if(p<0)continue;if(!NpcGripMath::ComposeMount(cache.target_local[t],cs[static_cast<size_t>(p)],cs[t]))return false;}
    double error=0;for(size_t i=0;i<3;++i){const double d=cs[static_cast<size_t>(k.palm_l)].translation[i]-goal_palm[i];error+=d*d;}
    cache.contact_error_max=(std::max)(cache.contact_error_max,std::sqrt(error));cache.contact_weight=weight;++cache.contacts;return true;
}
inline double Degrees(const double* q){return 2*std::acos((std::min)(1.0,std::abs(q[3])))*57.29577951308232;}
}
// Retarget pose. Turns target bone t's rest pose by `turn` (component space); its mapped
// descendants turn with it unless they get a turn of their own. Finish with FinishRestTurns.
inline bool SetRestTurn(Plan& plan,int32_t t,const double* turn) {
    const size_t n=plan.target.size();if(t<0||static_cast<size_t>(t)>=n||plan.target_to_source[static_cast<size_t>(t)]<0)return false;
    double length=0;for(size_t i=0;i<4;++i){if(!std::isfinite(turn[i]))return false;length+=turn[i]*turn[i];}if(std::abs(length-1)>0.001)return false;
    if(plan.rest_set.size()!=n){plan.rest_set.assign(n,0);plan.rest_align.assign(n,{0,0,0,1});}
    std::copy(turn,turn+4,plan.rest_align[static_cast<size_t>(t)].begin());plan.rest_set[static_cast<size_t>(t)]=1;return true;
}
// The nearest bone at or above t that has a turn of its own, or -1.
inline int32_t RestOwner(const Plan& plan,int32_t t) {
    if(plan.rest_set.size()!=plan.target.size())return -1;for(int32_t b=t;b>=0;b=plan.target[static_cast<size_t>(b)].parent)if(plan.rest_set[static_cast<size_t>(b)])return b;return -1;
}
// Turns t so its rest direction toward t_child matches the source's same two bones: the turn
// it inherits from an aligned ancestor, then the shortest arc that is still missing. Align a
// chain from the shoulder down and each joint keeps its own hinge. degrees = that last arc.
inline bool AlignRestDirection(Plan& plan,int32_t t,int32_t t_child,double* degrees=nullptr) {
    const int32_t n=static_cast<int32_t>(plan.target.size());if(t<0||t_child<0||t>=n||t_child>=n)return false;
    const int32_t s=plan.target_to_source[static_cast<size_t>(t)],sc=plan.target_to_source[static_cast<size_t>(t_child)];if(s<0||sc<0)return false;
    double inherited[4]={0,0,0,1};const int32_t owner=RestOwner(plan,plan.target[static_cast<size_t>(t)].parent);if(owner>=0)std::copy(plan.rest_align[static_cast<size_t>(owner)].begin(),plan.rest_align[static_cast<size_t>(owner)].end(),inherited);
    double from[3],turned[3],to[3],swing[4],turn[4];
    for(size_t i=0;i<3;++i){from[i]=plan.target_ref_cs[static_cast<size_t>(t_child)].translation[i]-plan.target_ref_cs[static_cast<size_t>(t)].translation[i];
        to[i]=plan.source_ref_cs[static_cast<size_t>(sc)].translation[i]-plan.source_ref_cs[static_cast<size_t>(s)].translation[i];}
    NpcGripMath::Rotate(inherited,from,turned);if(!detail::FromTo(turned,to,swing))return false;if(degrees)*degrees=detail::Degrees(swing);
    NpcGripMath::Mul(swing,inherited,turn);return SetRestTurn(plan,t,turn);
}
// Resolves inherited turns into rest_turn / rest_mode and the target's rest pose under them.
inline bool FinishRestTurns(Plan& plan) {
    const size_t n=plan.target.size();plan.rest_mode.assign(n,0);plan.rest_turn.assign(n,{0,0,0,1});plan.target_rest_cs=plan.target_ref_cs;
    if(plan.rest_set.size()!=n)return true;
    std::vector<int32_t> owner(n,-1);
    for(size_t t=0;t<n;++t){const int32_t p=plan.target[t].parent;owner[t]=plan.rest_set[t]?static_cast<int32_t>(t):p>=0?owner[static_cast<size_t>(p)]:-1;if(owner[t]<0)continue;
        Transform local=plan.target[t].ref;const int32_t s=plan.target_to_source[t];
        if(s>=0){double turned[4],inverse[4];NpcGripMath::Mul(plan.rest_align[static_cast<size_t>(owner[t])].data(),plan.target_ref_cs[t].rotation,turned);
            NpcGripMath::Inverse(plan.source_ref_cs[static_cast<size_t>(s)].rotation,inverse);NpcGripMath::Mul(inverse,turned,plan.rest_turn[t].data());plan.rest_mode[t]=1;
            if(p<0){std::memcpy(plan.target_rest_cs[t].rotation,turned,sizeof(turned));continue;}
            NpcGripMath::Inverse(plan.target_rest_cs[static_cast<size_t>(p)].rotation,inverse);NpcGripMath::Mul(inverse,turned,local.rotation);}
        else plan.rest_mode[t]=2;
        if(p>=0&&!NpcGripMath::ComposeMount(local,plan.target_rest_cs[static_cast<size_t>(p)],plan.target_rest_cs[t]))return false;}
    return true;
}
// foreign (0..1): weight of a montage authored for another skeleton (a boss skill or grab on the hero).
// The engine gives the source that skeleton's bone offsets, remapped (q0 inverse) and rescaled to the
// source's lengths (AnimationScaled) or as they are (Animation), so their difference from the source
// rest pose is the two skeletons' build, not motion, and added to the target it stretches the body.
// own: the montage is the target's own skeleton (an NPC skill on its skin). The engine played its key
// on the source by the source skeleton's mode (source_mode: Scaled key * source rest / target Skeleton
// rest length, Animation the key, Skeleton the source rest), so q0 * input gives the key back; the
// target shows it as the boss itself does, by its own Skeleton's mode on the display mesh (own_mode:
// Scaled key * display / Skeleton rest length, Animation the key, Skeleton the display rest). A display
// mesh built unlike its animation Skeleton (MengHua: pelvis 163 cm on a 102 cm Skeleton, every bone
// Animation) stretched when every bone counted as Scaled. A key the source lost (its Skeleton bones, a
// scaled bone of zero source length) is the Skeleton's rest length along the display rest. The boss
// weapon bone then moves as in the boss's own performance.
// Otherwise bones other than foreign_keep blend to the target's own rest translation by the weight.
inline double Length3(const double* v){return std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);}
inline void OwnTranslation(const Plan& plan,size_t t,const Transform& input,const Transform& ref,double* out) {
    constexpr double kSmall=1e-4; // UE_KINDA_SMALL_NUMBER: the engine leaves a translation unscaled below it
    const uint8_t mode=plan.source_mode.size()==plan.target.size()?plan.source_mode[t]:kScaled;
    const bool own_known=plan.own_mode.size()==plan.target.size()&&plan.own_length.size()==plan.target.size();
    const uint8_t own=own_known?plan.own_mode[t]:kScaled;
    const double* rest=plan.target[t].ref.translation;
    const double source_length=Length3(ref.translation),display_length=Length3(rest),skeleton_length=own_known?plan.own_length[t]:display_length;
    if(own==kSkeleton){for(size_t i=0;i<3;++i)out[i]=rest[i];return;}
    const bool rescaled=mode==kScaled&&skeleton_length>kSmall;
    if(mode==kSkeleton||(rescaled&&source_length<=1e-6)){
        const double k=own==kAnimation&&display_length>1e-6?skeleton_length/display_length:1;for(size_t i=0;i<3;++i)out[i]=rest[i]*k;return;}
    NpcGripMath::Rotate(plan.q0[t].data(),input.translation,out);
    double k=rescaled?skeleton_length/source_length:1;
    if(own==kScaled&&skeleton_length>kSmall)k*=display_length/skeleton_length;
    for(size_t i=0;i<3;++i)out[i]*=k;
}
namespace detail {
// a toward b by w: rotation slerp, translation and scale lerp.
inline void Blend(const Transform& a,const Transform& b,double w,Transform& out) {
    Transform r;Slerp(a.rotation,b.rotation,w,r.rotation);for(size_t i=0;i<3;++i){r.translation[i]=a.translation[i]+(b.translation[i]-a.translation[i])*w;r.scale[i]=a.scale[i]+(b.scale[i]-a.scale[i])*w;}out=r;
}
}
// puppet (Plan::puppet_always set): always-bones take the puppet's local pose (its root-relative pose,
// crossfaded over kPuppetBlendMs when its animation changes), other raw bones but the root by
// PuppetPose::body or, on a puppet_own plan, the own-montage weight, whichever is more. Bones above which
// the puppet turned the body keep the retarget-pose turns relative to it (Cache::shift). Unmapped bones
// on a spare slot (Plan::extra_source) reach the CopyPose through it.
inline bool Evaluate(const Plan& plan,const Transform* source,size_t count,Cache& cache,const char*& why,double foreign=0,bool own=false,const PuppetPose* puppet=nullptr) {
    if(!source||count!=plan.source.size()||cache.snapshot.size()!=count||cache.target_cs.size()!=plan.target.size()){why="pose-count/epoch";return false;}
    foreign=std::isfinite(foreign)?std::clamp(foreign,0.0,1.0):0.0;own=own&&foreign>0;const size_t n=plan.target.size();
    bool puppeted=puppet&&puppet->cs&&puppet->count==n&&plan.puppet_always.size()==n&&cache.puppet_last.size()==n&&cache.shift.size()==n;double fade=1;
    // A puppet not evaluated yet (or mid-teardown) is ignored this time, never a failure.
    for(size_t t=0;puppeted&&t<plan.target_raw;++t){const Transform& v=puppet->cs[t];puppeted=NpcGripMath::FiniteMount(v)&&v.scale[0]>=0&&v.scale[1]>=0&&v.scale[2]>=0;}
    if(puppeted){
        if(!cache.puppet_seen){cache.puppet_seen=true;cache.puppet_serial=puppet->serial;cache.puppet_switch_ms=0;}
        else if(puppet->serial!=cache.puppet_serial){cache.puppet_serial=puppet->serial;cache.puppet_from=cache.puppet_last;cache.puppet_switch_ms=puppet->now_ms?puppet->now_ms:1;}
        if(cache.puppet_switch_ms){const uint64_t age=puppet->now_ms>cache.puppet_switch_ms?puppet->now_ms-cache.puppet_switch_ms:0;
            if(age>=kPuppetBlendMs)cache.puppet_switch_ms=0;else fade=static_cast<double>(age)/static_cast<double>(kPuppetBlendMs);}}
    else cache.puppet_seen=false;
    const double body=puppeted&&std::isfinite(puppet->body)?std::clamp(puppet->body,0.0,1.0):0.0;
    const bool puppet_same=!puppeted||(fade>=1&&cache.puppet_used&&!cache.puppet_faded&&!std::memcmp(puppet->cs,cache.puppet_snapshot.data(),n*sizeof(Transform)));
    if(cache.ready&&cache.foreign==foreign&&cache.own==own&&cache.puppet_used==puppeted&&cache.puppet_body==body&&puppet_same&&!std::memcmp(source,cache.snapshot.data(),count*sizeof(Transform))){++cache.hits;return true;}
    cache.ready=false;cache.foreign=foreign;cache.own=own;cache.puppet_used=false;cache.puppet_body=body;const bool keeps=plan.foreign_keep.size()==n;
    const auto puppet_weight=[&](size_t t){return puppeted&&t>0&&t<plan.target_raw?(plan.puppet_always[t]?1.0:(std::max)(plan.puppet_own&&own?foreign:0.0,body)):0.0;};
    // All storage is prepared before the session. This path allocates nothing
    // and never edits source or an engine pose array.
    for(size_t s=0;s<count;++s){if(plan.source_to_target[s]<0)continue;const int32_t p=plan.source[s].parent;if(!NpcGripMath::FiniteMount(source[s])){why="source-pose-invalid";return false;}for(size_t axis=0;axis<3;++axis)if(source[s].scale[axis]<0){why="shared-negative-scale-native-matrix-required";return false;}
        if(p<0)cache.source_local[s]=source[s];else if(!NpcGripMath::RelativeNativeSafe(source[s],source[static_cast<size_t>(p)],cache.source_local[s])){why="source-local-invalid";return false;}}
    for(size_t t=0;t<plan.target.size();++t){Transform local=plan.target[t].ref;const int32_t s=plan.target_to_source[t];
        if(s>=0){const Transform& input=cache.source_local[static_cast<size_t>(s)],&ref=plan.source[static_cast<size_t>(s)].ref;double tmp[4],delta[3],rotated[3];
            NpcGripMath::Mul(plan.q0[t].data(),input.rotation,tmp);NpcGripMath::Mul(tmp,plan.q1[t].data(),local.rotation);
            for(size_t i=0;i<3;++i)delta[i]=input.translation[i]-ref.translation[i];NpcGripMath::Rotate(plan.q0[t].data(),delta,rotated);
            if(own){double back[3];OwnTranslation(plan,t,input,ref,back);
                for(size_t i=0;i<3;++i){local.translation[i]=(local.translation[i]+rotated[i])*(1-foreign)+back[i]*foreign;local.scale[i]=input.scale[i]*local.scale[i]/ref.scale[i];}}
            else{const double motion=keeps&&plan.foreign_keep[t]?1:1-foreign;
                for(size_t i=0;i<3;++i){local.translation[i]+=rotated[i]*motion;local.scale[i]=input.scale[i]*local.scale[i]/ref.scale[i];}}}
        const int32_t p=plan.target[t].parent;
        if(puppeted){double* shift=cache.shift[t].data();if(p>=0)std::memcpy(shift,cache.shift[static_cast<size_t>(p)].data(),4*sizeof(double));else{shift[0]=shift[1]=shift[2]=0;shift[3]=1;}
            if(t>0&&t<plan.target_raw&&p>=0){Transform& mine=cache.puppet_last[t];
                if(!NpcGripMath::RelativeNativeSafe(puppet->cs[t],puppet->cs[static_cast<size_t>(p)],mine)){why="puppet-local-invalid";return false;}
                if(fade<1)detail::Blend(cache.puppet_from[t],mine,fade,mine);
                const double w=puppet_weight(t);
                if(w>0){double before[4],after[4],inverse[4],turn[4],next[4];const double* parent=cache.target_cs[static_cast<size_t>(p)].rotation;NpcGripMath::Mul(parent,local.rotation,before);
                    detail::Blend(local,mine,w,local);NpcGripMath::Mul(parent,local.rotation,after);
                    NpcGripMath::Inverse(before,inverse);NpcGripMath::Mul(after,inverse,turn);NpcGripMath::Mul(turn,shift,next);std::memcpy(shift,next,sizeof(next));}}}
        cache.target_local[t]=local;
        if(p<0)cache.target_cs[t]=local;else if(!NpcGripMath::ComposeMount(local,cache.target_cs[static_cast<size_t>(p)],cache.target_cs[t])){why="target-compose-invalid";return false;}}
    // Retarget pose: turned bones take source CS * rest_turn (turned further by what the puppet turned
    // above them); bones under them follow. The target's own montage, and a bone the puppet holds, keep
    // their own rotation instead: that key is already the target's pose (its own arms on its weapon).
    if(plan.rest_mode.size()==n)for(size_t t=0;t<n;++t){if(!plan.rest_mode[t])continue;const int32_t p=plan.target[t].parent;
        Transform& local=cache.target_local[t];
        if(plan.rest_mode[t]==1){double want[4],turned[4];NpcGripMath::Mul(source[static_cast<size_t>(plan.target_to_source[t])].rotation,plan.rest_turn[t].data(),turned);
            if(puppeted&&p>=0){double tmp[4];NpcGripMath::Mul(cache.shift[static_cast<size_t>(p)].data(),turned,tmp);std::memcpy(turned,tmp,sizeof(tmp));}
            const double native=(std::max)(own?foreign:0.0,puppet_weight(t));
            if(native>0)detail::Slerp(turned,cache.target_cs[t].rotation,native,want);else std::memcpy(want,turned,sizeof(want));
            if(p<0){std::memcpy(cache.target_cs[t].rotation,want,sizeof(want));std::memcpy(local.rotation,want,sizeof(want));continue;}
            double inverse[4];NpcGripMath::Inverse(cache.target_cs[static_cast<size_t>(p)].rotation,inverse);NpcGripMath::Mul(inverse,want,local.rotation);}
        if(p>=0&&!NpcGripMath::ComposeMount(local,cache.target_cs[static_cast<size_t>(p)],cache.target_cs[t])){why="rest-turn-compose-invalid";return false;}}
    if(!detail::SolveHandContact(plan,source,cache)){why="hand-contact-compose-invalid";return false;}
    // IK copies take their FK bone's final transform (after rest turns and the hand contact).
    if(plan.follow.size()==plan.target.size()&&cache.moved.size()==plan.target.size())for(size_t t=0;t<plan.target.size();++t){
        const int32_t p=plan.target[t].parent,f=plan.follow[t];cache.moved[t]=0;
        if(f>=0){Transform followed;
            if(!NpcGripMath::ComposeMount(plan.follow_rel[t],cache.target_cs[static_cast<size_t>(f)],followed)){why="ik-follow-compose-invalid";return false;}
            const double authored=own&&plan.authored_follow.size()==n&&plan.authored_follow[t]?foreign:0;
            detail::Blend(followed,cache.target_cs[t],authored,cache.target_cs[t]);
            if(p<0)cache.target_local[t]=cache.target_cs[t];else if(!NpcGripMath::RelativeNativeSafe(cache.target_cs[t],cache.target_cs[static_cast<size_t>(p)],cache.target_local[t])){why="ik-follow-local-invalid";return false;}
            cache.moved[t]=1;}
        else if(p>=0&&cache.moved[static_cast<size_t>(p)]){if(!NpcGripMath::ComposeMount(cache.target_local[t],cache.target_cs[static_cast<size_t>(p)],cache.target_cs[t])){why="ik-follow-child-invalid";return false;}cache.moved[t]=1;}}
    std::memcpy(cache.synthetic_source.data(),source,count*sizeof(Transform));
    for(size_t s=0;s<count;++s)if(plan.source_to_target[s]>=0)cache.synthetic_source[s]=cache.target_cs[static_cast<size_t>(plan.source_to_target[s])];
    // A spare slot's CopyPose local is the slot relative to its source parent's slot (parents first); a
    // component-space CopyPose (Plan::extra_mesh) copies the slot as the bone's component-space transform.
    if(plan.extra_source.size()==n)for(const int32_t t:plan.extra_order){const size_t s=static_cast<size_t>(plan.extra_source[static_cast<size_t>(t)]);const int32_t ps=plan.source[s].parent;
        if(s>=count||ps<0){why="extra-slot-compose-invalid";return false;}
        if(plan.extra_mesh){cache.synthetic_source[s]=cache.target_cs[static_cast<size_t>(t)];continue;}
        if(!NpcGripMath::ComposeMount(cache.target_local[static_cast<size_t>(t)],cache.synthetic_source[static_cast<size_t>(ps)],cache.synthetic_source[s])){why="extra-slot-compose-invalid";return false;}}
    if(puppeted){std::memcpy(cache.puppet_snapshot.data(),puppet->cs,n*sizeof(Transform));cache.puppet_used=true;cache.puppet_faded=fade<1;++cache.puppet_builds;}
    std::memcpy(cache.snapshot.data(),source,count*sizeof(Transform));cache.ready=true;++cache.builds;why="ok";return true;
}
#ifdef _MSC_VER
#pragma warning(pop)
#endif
}
