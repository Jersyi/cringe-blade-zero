#pragma once
#include "npc_grip_math.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <locale>
#include <sstream>
#include <string>
#include <vector>
#include <cctype>

// Pure, bounded bone edits. Angles use degrees, translations cm, scales multipliers.
// Motion 1 follows acceleration; 2 adds a small pelvis turn from the authored pose; 3 counter-turns the chest.
namespace BoneMotion {
struct alignas(16) Transform { double rotation[4]{0,0,0,1},translation[4]{},scale[4]{1,1,1,0}; };
struct Row {
    std::string bone="pelvis";
    std::array<double,3> translation{},rotation{},scale{1,1,1};
    int motion=0; double strength=1,limit=1.5,frequency=2.5,damping=0.8;
    bool on=true;
    int children=0; // 0 inherit; 1 follow shape, keep child size; 2 preserve descendants' component pose
};
struct Config { bool on=false,protect_feet=true;std::vector<Row> rows; };
inline bool Supplemental(std::string name){
    for(char& c:name)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if(name.rfind("pbz_",0)==0)name.erase(0,4);
    return name=="breast_l"||name=="breast_r"||name=="butt_l"||name=="butt_r";
}
// Original bones stay on the animation source so head, hair, clothes and weapon attachments follow.
// Only the four added bones are edited after the display has copied that source pose.
inline std::array<Config,2> SplitSupplement(const Config& config){
    std::array<Config,2> result{config,config};for(auto& c:result)c.rows.clear();
    for(const auto& r:config.rows)result[Supplemental(r.bone)?1:0].rows.push_back(r);
    for(auto& c:result)c.on=config.on&&!c.rows.empty();return result;
}
constexpr size_t MaxRows=32;
inline bool Facial(const std::string& name){
    std::string n=name;for(char& c:n)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for(const char* prefix:{"facial_","face_","jaw","tongue","teeth","eyelid","eyebrow","eye_l","eye_r","lip_","nose_"})if(n.rfind(prefix,0)==0)return true;
    return false;
}
inline double Clamp(double v,double lo,double hi,double fallback=0){return std::isfinite(v)?std::clamp(v,lo,hi):fallback;}
inline bool Name(const std::string& s){
    if(s.empty()||s.size()>96)return false;
    for(unsigned char c:s)if(c<32||c>126||c=='|'||c==';'||c==','||c=='~'||c==':')return false;
    return true;
}
inline Config Clean(Config c){
    if(c.rows.size()>MaxRows)c.rows.resize(MaxRows);
    c.rows.erase(std::remove_if(c.rows.begin(),c.rows.end(),[](const Row& r){return !Name(r.bone);}),c.rows.end());
    for(Row& r:c.rows){
        for(size_t i=0;i<3;++i){r.translation[i]=Clamp(r.translation[i],-100,100);r.rotation[i]=Clamp(r.rotation[i],-180,180);r.scale[i]=Clamp(r.scale[i],0.1,3,1);}
        r.motion=std::clamp(r.motion,0,3);r.strength=Clamp(r.strength,0,5,1);r.limit=Clamp(r.limit,0,15,1.5);
        r.frequency=Clamp(r.frequency,0.5,10,2.5);r.damping=Clamp(r.damping,0.1,2,0.8);
        r.children=std::clamp(r.children,0,2);
    }return c;
}
inline std::string Encode(Config c){
    c=Clean(std::move(c));std::ostringstream s;s.imbue(std::locale::classic());s.precision(8);s<<(c.on?1:0)<<','<<(c.protect_feet?1:0);
    for(const Row& r:c.rows){s<<';'<<r.bone<<','<<(r.on?1:0);for(double v:r.translation)s<<','<<v;for(double v:r.rotation)s<<','<<v;for(double v:r.scale)s<<','<<v;
        s<<','<<r.motion<<','<<r.strength<<','<<r.limit<<','<<r.frequency<<','<<r.damping<<','<<r.children;}
    return s.str();
}
inline Config Decode(const std::string& text){
    Config c;if(text.size()>16000)return c;std::istringstream all(text);all.imbue(std::locale::classic());std::string line;
    if(!std::getline(all,line,';'))return c;
    {std::replace(line.begin(),line.end(),',',' ');std::istringstream s(line);int on=0,feet=1;if(!(s>>on>>feet))return c;c.on=on==1;c.protect_feet=feet==1;}
    while(c.rows.size()<MaxRows&&std::getline(all,line,';')){
        const auto comma=line.find(',');if(comma==std::string::npos)continue;Row r;r.bone=line.substr(0,comma);
        line=line.substr(comma+1);std::replace(line.begin(),line.end(),',',' ');std::istringstream s(line);s.imbue(std::locale::classic());int on=0;
        if(!(s>>on))continue;r.on=on==1;for(double& v:r.translation)s>>v;for(double& v:r.rotation)s>>v;for(double& v:r.scale)s>>v;
        if(!(s>>r.motion>>r.strength>>r.limit>>r.frequency>>r.damping))continue;
        s>>std::ws;if(!s.eof()&&!(s>>r.children))continue;std::string extra;if(s>>extra)continue;c.rows.push_back(r);
    }return Clean(std::move(c));
}
inline Config Feminine(){
    Config c;c.on=true;
    Row hip;hip.motion=2;hip.strength=0.65;c.rows.push_back(hip);
    Row hip_follow;hip_follow.motion=1;hip_follow.strength=0.2;hip_follow.limit=0.35;hip_follow.frequency=3;hip_follow.damping=1;c.rows.push_back(hip_follow);
    Row chest;chest.bone="spine_03";chest.motion=3;chest.strength=0.4;c.rows.push_back(chest);
    for(const char* name:{"breast_l","breast_r","butt_l","butt_r"}){Row r;r.bone=name;r.motion=1;r.strength=1;r.limit=name[0]=='b'&&name[1]=='r'?3.5:2.5;r.frequency=2.5;r.damping=0.6;c.rows.push_back(r);}
    return c;
}
struct Spring {
    std::array<double,3> last{},velocity{},offset{},speed{};bool ready=false;
    void Reset(){*this={};}
    void Step(const std::array<double,3>& target,double dt,const Row& row){
        // A second Finalize in the same sample must reuse, not erase, the spring.
        if(ready&&std::isfinite(dt)&&dt>=0&&dt<0.001)return;
        double distance=0;for(size_t i=0;i<3;++i)distance+=std::pow(target[i]-last[i],2);
        if(!ready||!std::isfinite(dt)||dt<0||dt>0.1||distance>10000){Reset();last=target;ready=true;return;}
        const double w=6.283185307179586*row.frequency;std::array<double,3> acceleration{};
        for(size_t i=0;i<3;++i){const double v=(target[i]-last[i])/dt;acceleration[i]=Clamp((v-velocity[i])/dt,-6000,6000);velocity[i]=v;last[i]=target[i];}
        const int steps=static_cast<int>(std::ceil(dt*240));const double h=dt/steps;
        for(int n=0;n<steps;++n)for(size_t i=0;i<3;++i){speed[i]+=(-w*w*offset[i]-2*row.damping*w*speed[i]-0.08*(Supplemental(row.bone)?3.0:1.0)*row.strength*acceleration[i])*h;offset[i]+=speed[i]*h;
            if(std::abs(offset[i])>row.limit){offset[i]=std::copysign(row.limit,offset[i]);speed[i]=0;}}
    }
};
inline void Euler(const std::array<double,3>& degrees,double q[4]){
    const double k=3.141592653589793/360;double axis[3][4]={{std::sin(degrees[0]*k),0,0,std::cos(degrees[0]*k)},{0,std::sin(degrees[1]*k),0,std::cos(degrees[1]*k)},{0,0,std::sin(degrees[2]*k),std::cos(degrees[2]*k)}};
    double t[4];NpcGripMath::Mul(axis[2],axis[1],t);NpcGripMath::Mul(t,axis[0],q);
}
struct Binding { int bone=-1;Spring spring; };
struct MotionClock {
    double last=0;bool ready=false;
    double Advance(double now){
        if(!ready){last=now;ready=true;return 0;}
        const double dt=now-last;
        if(dt>=0&&dt<0.001)return 0; // repeated callbacks accumulate time to the next real sample
        last=now;return dt;
    }
};
inline std::vector<Binding> Rebind(const Config& previous,const std::vector<Binding>& old,const Config& next,const std::vector<int>& bones){
    std::vector<Binding> result(next.rows.size());std::vector<bool> used(old.size(),false);
    for(size_t i=0;i<result.size();++i){auto& b=result[i];b.bone=i<bones.size()?bones[i]:-1;const Row& row=next.rows[i];
        if(!previous.on||!next.on||!row.on||row.motion!=1||b.bone<0)continue;
        for(size_t j=0;j<old.size()&&j<previous.rows.size();++j)if(!used[j]&&old[j].bone==b.bone&&previous.rows[j].on&&previous.rows[j].motion==1){
            used[j]=true;b.spring=old[j].spring;
            for(size_t k=0;k<3;++k)if(std::abs(b.spring.offset[k])>row.limit){b.spring.offset[k]=std::copysign(row.limit,b.spring.offset[k]);b.spring.speed[k]=0;}
            break;
        }
    }return result;
}
// Builds from a fresh evaluated pose every time. No output is used as the next input; missing bones are skipped.
inline bool Apply(const Config& c,const std::vector<int>& parents,const std::vector<Transform>& reference,
                  const std::vector<Transform>& input,const Transform& component,std::vector<Binding>& bindings,
                  const std::vector<int>& protected_roots,int pelvis,double dt,std::vector<Transform>& output){
    if(!c.on||input.empty()||input.size()!=parents.size()||reference.size()!=input.size()||bindings.size()!=c.rows.size())return false;
    std::vector<Transform> locals(input.size());
    for(size_t i=0;i<input.size();++i){const int p=parents[i];if(p>=static_cast<int>(i)||p< -1||!NpcGripMath::FiniteMount(input[i]))return false;
        if(p<0)locals[i]=input[i];else if(!NpcGripMath::RelativeNativeSafe(input[i],input[static_cast<size_t>(p)],locals[i]))return false;}
    double pelvis_turn=0;
    if(pelvis>=0&&static_cast<size_t>(pelvis)<locals.size()){
        double inv[4],delta[4];NpcGripMath::Inverse(reference[static_cast<size_t>(pelvis)].rotation,inv);NpcGripMath::Mul(locals[static_cast<size_t>(pelvis)].rotation,inv,delta);
        pelvis_turn=Clamp(2*std::atan2(delta[0],std::abs(delta[3]))*180/3.141592653589793,-8,8);
    }
    bool any=false;
    for(size_t r=0;r<c.rows.size();++r){const Row& row=c.rows[r];Binding& binding=bindings[r];const int b=binding.bone;
        if(!row.on||b<0||static_cast<size_t>(b)>=locals.size()){binding.spring.Reset();continue;}any=true;
        std::vector<Transform> before;
        if(row.children==2){before.resize(input.size());for(size_t i=0;i<input.size();++i){const int p=parents[i];if(p<0)before[i]=locals[i];else if(!NpcGripMath::ComposeMount(locals[i],before[static_cast<size_t>(p)],before[i]))return false;}}
        Transform& local=locals[static_cast<size_t>(b)];std::array<double,3> rotation=row.rotation;
        if(row.motion==2)rotation[0]+=pelvis_turn*row.strength;
        if(row.motion==3)rotation[0]-=pelvis_turn*row.strength;
        double q[4],turned[4];Euler(rotation,q);NpcGripMath::Mul(local.rotation,q,turned);for(size_t k=0;k<4;++k)local.rotation[k]=turned[k];
        for(size_t k=0;k<3;++k){local.translation[k]+=row.translation[k];local.scale[k]*=row.scale[k];}
        if(row.motion==1){
            Transform world;NpcGripMath::ComposeMount(input[static_cast<size_t>(b)],component,world);
            binding.spring.Step({world.translation[0],world.translation[1],world.translation[2]},dt,row);
            Transform parent=component;const int p=parents[static_cast<size_t>(b)];if(p>=0)NpcGripMath::ComposeMount(input[static_cast<size_t>(p)],component,parent);
            double inv[4],delta[3];NpcGripMath::Inverse(parent.rotation,inv);NpcGripMath::Rotate(inv,binding.spring.offset.data(),delta);
            for(size_t k=0;k<3;++k)if(std::abs(parent.scale[k])>1e-6)local.translation[k]+=delta[k]/parent.scale[k];
        }else binding.spring.Reset();
        if(row.children==1){for(size_t i=0;i<locals.size();++i)if(parents[i]==b)for(size_t k=0;k<3;++k){if(row.scale[k]<=0)return false;locals[i].scale[k]/=row.scale[k];}}
        if(row.children==2){const int p=parents[static_cast<size_t>(b)];Transform changed=local;if(p>=0&&!NpcGripMath::ComposeMount(local,before[static_cast<size_t>(p)],changed))return false;
            for(size_t i=0;i<locals.size();++i)if(parents[i]==b&&!NpcGripMath::RelativeNativeSafe(before[i],changed,locals[i]))return false;}
    }
    if(!any)return false;output.resize(input.size());std::vector<bool> keep(input.size(),false);std::vector<size_t> roots(input.size());
    for(size_t i=0;i<input.size();++i){const int p=parents[i];keep[i]=c.protect_feet&&(std::find(protected_roots.begin(),protected_roots.end(),static_cast<int>(i))!=protected_roots.end()||(p>=0&&keep[static_cast<size_t>(p)]));
        roots[i]=p<0?i:roots[static_cast<size_t>(p)];
        // Feet protection removes pelvis edits, not an intentional whole-character transform.
        if(keep[i]&&p>=0){Transform relative;if(!NpcGripMath::RelativeNativeSafe(input[i],input[roots[i]],relative)||!NpcGripMath::ComposeMount(relative,output[roots[i]],output[i]))return false;}
        else if(p<0)output[i]=locals[i];else if(!NpcGripMath::ComposeMount(locals[i],output[static_cast<size_t>(p)],output[i]))return false;
        if(!NpcGripMath::FiniteMount(output[i]))return false;
    }return true;
}
}
