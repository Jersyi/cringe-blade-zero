#pragma once
#include "bone_motion_model.h"

// Original PBZ editor helpers. UI concepts were referenced from UUEPBS;
// no upstream source or bone dictionary is redistributed (see THIRD_PARTY_NOTICES.md).
namespace BoneBodyEditor {
inline std::string Lower(std::string s){for(char& c:s)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));return s;}
struct Group {const wchar_t* label;const char* prefix;};
inline constexpr Group Groups[]={{L"头部","head"},{L"颈部","neck_"},{L"胸腔","spine_03"},{L"腰部","spine_01"},{L"胯部","pelvis"},
    {L"肩部","clavicle_"},{L"上臂","upperarm_"},{L"前臂","lowerarm_"},{L"手掌","hand_"},{L"胸部软组织","breast_"},
    {L"臀部软组织","butt_"},{L"大腿","thigh_"},{L"小腿","calf_"},{L"脚部","foot_"},{L"全身","root"}};
inline bool Matches(const std::string& bone,const Group& group){
    if(BoneMotion::Facial(bone))return false;std::string n=Lower(bone);const std::string p=group.prefix;
    if(n.rfind("pbz_breast_",0)==0||n.rfind("pbz_butt_",0)==0)n=n.substr(4);
    if(p=="butt_")return n=="butt_l"||n=="butt_r"||n=="glute_l"||n=="glute_r";
    // Explicit deform bones only. Twist/corrective chains inherit; do not multiply
    // every descendant's scale, which would compound a single body slider.
    if(p=="neck_")return n=="neck_01";
    if(!p.empty()&&p.back()=='_')return n==p+"l"||n==p+"r";
    return n==p;
}
inline int FindRow(const BoneMotion::Config& c,const std::string& bone){for(size_t i=0;i<c.rows.size();++i)if(Lower(c.rows[i].bone)==Lower(bone)&&c.rows[i].motion==0)return static_cast<int>(i);return -1;}
inline std::vector<std::string> Members(const std::vector<std::string>& bones,size_t group){std::vector<std::string> out;if(group>=std::size(Groups))return out;for(const auto& n:bones)if(Matches(n,Groups[group]))out.push_back(n);return out;}
inline bool SetGroup(BoneMotion::Config& c,const std::vector<std::string>& bones,size_t group,const std::array<double,3>& scale){
    const auto members=Members(bones,group);if(members.empty())return false;size_t add=0;for(const auto& n:members)if(FindRow(c,n)<0)++add;if(c.rows.size()+add>BoneMotion::MaxRows)return false;
    for(const auto& n:members){int at=FindRow(c,n);if(at<0){BoneMotion::Row r;r.bone=n;c.rows.push_back(r);at=static_cast<int>(c.rows.size())-1;}auto& r=c.rows[static_cast<size_t>(at)];r.scale=scale;r.on=true;}
    c=BoneMotion::Clean(c);return true;
}
inline std::string Twin(const std::string& name,const std::vector<std::string>& bones){std::string n=Lower(name);if(n.size()<3)return {};const std::string end=n.substr(n.size()-2);if(end!="_l"&&end!="_r")return {};n.back()=end=="_l"?'r':'l';for(const auto& b:bones)if(Lower(b)==n)return b;return {};}
inline void Angles(const double q[4],std::array<double,3>& degrees){const double k=180/3.141592653589793;degrees={std::atan2(2*(q[3]*q[0]+q[1]*q[2]),1-2*(q[0]*q[0]+q[1]*q[1]))*k,
    std::asin(std::clamp(2*(q[3]*q[1]-q[2]*q[0]),-1.0,1.0))*k,std::atan2(2*(q[3]*q[2]+q[0]*q[1]),1-2*(q[1]*q[1]+q[2]*q[2]))*k};}
inline bool Mirror(const BoneMotion::Row& source,const std::vector<std::string>& names,const std::vector<int>& parents,const std::vector<BoneMotion::Transform>& ref,BoneMotion::Row& target){
    const auto twin=Twin(source.bone,names);if(twin.empty()||names.size()!=parents.size()||ref.size()!=names.size())return false;
    std::vector<BoneMotion::Transform> cs(ref.size());int a=-1,b=-1;
    for(size_t i=0;i<ref.size();++i){if(parents[i]<-1||parents[i]>=static_cast<int>(i)||!NpcGripMath::Valid(ref[i]))return false;if(parents[i]<0)cs[i]=ref[i];else if(!NpcGripMath::ComposeMount(ref[i],cs[static_cast<size_t>(parents[i])],cs[i]))return false;
        if(Lower(names[i])==Lower(source.bone))a=static_cast<int>(i);if(names[i]==twin)b=static_cast<int>(i);}
    if(a<0||b<0)return false;target=source;target.bone=twin;const auto& src=cs[static_cast<size_t>(a)];const auto& dst=cs[static_cast<size_t>(b)];
    double q[4],inv[4],temp[4],world[4];BoneMotion::Euler(source.rotation,q);NpcGripMath::Inverse(src.rotation,inv);NpcGripMath::Mul(src.rotation,q,temp);NpcGripMath::Mul(temp,inv,world);
    world[0]=-world[0];world[2]=-world[2];NpcGripMath::Inverse(dst.rotation,inv);NpcGripMath::Mul(inv,world,temp);NpcGripMath::Mul(temp,dst.rotation,q);Angles(q,target.rotation);
    BoneMotion::Transform identity;const auto& pa=parents[static_cast<size_t>(a)]<0?identity:cs[static_cast<size_t>(parents[static_cast<size_t>(a)])];const auto& pb=parents[static_cast<size_t>(b)]<0?identity:cs[static_cast<size_t>(parents[static_cast<size_t>(b)])];
    double moved[3],scaled[3];for(size_t k=0;k<3;++k)scaled[k]=source.translation[k]*pa.scale[k];NpcGripMath::Rotate(pa.rotation,scaled,moved);moved[1]=-moved[1];NpcGripMath::Inverse(pb.rotation,inv);NpcGripMath::Rotate(inv,moved,scaled);for(size_t k=0;k<3;++k)target.translation[k]=scaled[k]/pb.scale[k];
    // General non-uniform scale under a rotated mirror would require shear.
    // Accept only a signed axis permutation; uniform scale is always representable.
    if(std::abs(source.scale[0]-source.scale[1])<1e-8&&std::abs(source.scale[1]-source.scale[2])<1e-8)return true;
    NpcGripMath::Inverse(dst.rotation,inv);bool used[3]{};
    for(size_t k=0;k<3;++k){double axis[3]{},v[3],out[3];axis[k]=1;NpcGripMath::Rotate(src.rotation,axis,v);v[1]=-v[1];NpcGripMath::Rotate(inv,v,out);size_t best=0;for(size_t j=1;j<3;++j)if(std::abs(out[j])>std::abs(out[best]))best=j;if(std::abs(out[best])<.9999||used[best])return false;used[best]=true;target.scale[best]=source.scale[k];}
    return true;
}
}
