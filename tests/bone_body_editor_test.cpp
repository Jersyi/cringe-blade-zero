#include "bone_body_editor.h"
#include <cstdio>
int main(){using namespace BoneMotion;using namespace BoneBodyEditor;int checks=0,failed=0;auto check=[&](bool v){++checks;if(!v){++failed;std::printf("failed %d\n",checks);}};
    std::vector<std::string> names{"root","upperarm_l","upperarm_r","upperarm_twist_01_l","FACIAL_C_Jaw","breast_l","breast_r"};
    check(Members({"neck_01","neck_02","neck_twist_01"},1)==std::vector<std::string>{"neck_01"});
    Config c;c.on=true;c.protect_feet=false;check(SetGroup(c,names,6,{1.2,1.3,1.4}));check(c.rows.size()==2);check(c.rows[0].bone=="upperarm_l"&&c.rows[1].bone=="upperarm_r");check(!SetGroup(c,names,10,{2,2,2}));
    auto motion=Feminine();const auto old=motion.rows;check(SetGroup(motion,names,9,{1.1,1.1,1.1}));check(motion.rows.size()==old.size()+2);check(motion.rows[3].motion==1&&motion.rows[3].scale[0]==1);
    check(Twin("UPPERARM_L",names)=="upperarm_r");check(Twin("root",names).empty());
    std::vector<int> parents{-1,0,0,1,0,0,0};std::vector<Transform> ref(names.size());ref[1].translation[1]=-20;ref[2].translation[1]=20;
    Row source;source.bone="upperarm_l";source.translation={1,2,3};source.rotation={10,20,30};source.scale={1.1,1.2,1.3};source.motion=1;source.children=2;Row target,back;
    check(Mirror(source,names,parents,ref,target));check(target.bone=="upperarm_r"&&target.translation==std::array<double,3>{1,-2,3}&&target.motion==1&&target.children==2);
    check(Mirror(target,names,parents,ref,back));for(size_t k=0;k<3;++k){check(std::abs(back.rotation[k]-source.rotation[k])<1e-8);check(std::abs(back.scale[k]-source.scale[k])<1e-8);}
    // A different counterpart reference basis requires rotation/axis conversion.
    Euler({0,0,90},ref[2].rotation);check(Mirror(source,names,parents,ref,target));check(std::abs(target.scale[0]-source.scale[1])<1e-8&&std::abs(target.scale[1]-source.scale[0])<1e-8);
    check(Mirror(target,names,parents,ref,back));for(size_t k=0;k<3;++k)check(std::abs(back.rotation[k]-source.rotation[k])<1e-8);
    Euler({0,0,45},ref[2].rotation);check(!Mirror(source,names,parents,ref,target));source.scale={1.2,1.2,1.2};check(Mirror(source,names,parents,ref,target));
    const auto legacy="1,0;pelvis,1,0,0,0,0,0,0,1,1,1,0,1,1.5,2.5,0.8";check(Decode(legacy).rows.size()==1&&Decode(legacy).rows[0].children==0);
    for(int mode=0;mode<3;++mode){c.rows.resize(1);c.rows[0].children=mode;check(Decode(Encode(c)).rows[0].children==mode);}
    std::vector<int> p{-1,0,1,2};std::vector<Transform> r(4),in(4),out;r[1].translation[2]=10;r[2].translation[0]=5;r[3].translation[0]=3;in[0]=r[0];for(size_t i=1;i<4;++i)NpcGripMath::ComposeMount(r[i],in[i-1],in[i]);
    Config edit;edit.on=true;edit.protect_feet=false;Row row;row.scale={2,2,2};row.translation={1,0,0};row.rotation={0,0,30};edit.rows={row};std::vector<Binding> bindings(1);bindings[0].bone=1;
    for(int mode=0;mode<3;++mode){edit.rows[0].children=mode;for(int i=0;i<1000;++i){check(Apply(edit,p,r,in,Transform{},bindings,{},-1,1.0/60,out));check(std::abs(out[2].scale[0]-(mode==0?2:1))<1e-8);if(mode==2){check(std::abs(out[2].translation[0]-in[2].translation[0])<1e-8);check(std::abs(out[3].translation[0]-in[3].translation[0])<1e-8);}}}
    {
        Config full;full.on=true;full.protect_feet=true;const std::vector<std::string> body{"root","pelvis","thigh_l","foot_l"};
        check(SetGroup(full,body,std::size(Groups)-1,{1.5,1.5,1.5}));check(full.rows.size()==1&&full.rows[0].bone=="root");
        std::vector<Binding> all(1);all[0].bone=0;
        check(Apply(full,p,r,in,Transform{},all,{2},1,1.0/60,out));
        for(size_t i=0;i<out.size();++i){check(std::abs(out[i].scale[0]-1.5)<1e-8);check(std::abs(out[i].translation[2]-in[i].translation[2]*1.5)<1e-8);}
        Row hip;hip.translation[2]=20;full.rows.push_back(hip);all.push_back({});all[1].bone=1;
        check(Apply(full,p,r,in,Transform{},all,{2},1,1.0/60,out));check(std::abs(out[2].translation[2]-in[2].translation[2]*1.5)<1e-8);
        full.rows[0].scale={1,1,1};check(Apply(full,p,r,in,Transform{},all,{2},1,1.0/60,out));check(std::abs(out[2].translation[2]-in[2].translation[2])<1e-8);
        check(Encode(Decode(Encode(full)))==Encode(full));
    }
    std::printf("bone_body_editor checks=%d failures=%d\n",checks,failed);return failed?1:0;
}
