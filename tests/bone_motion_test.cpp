#include "bone_motion_model.h"
#include "bone_debug_projection.h"
#include "npc_combo_model.h"
#include <cstdio>
#include <limits>
int main(){using namespace BoneMotion;int checks=0,failed=0;const auto check=[&](bool ok){++checks;if(!ok){++failed;std::printf("FAILED check %d\n",checks);}};
    for(const char* name:{"FACIAL_C_Jaw","jaw","eye_l","tongue_01","eyebrow_r","face_root"})check(Facial(name));
    for(const char* name:{"head","neck_01","pelvis","breast_l","thigh_bck_l"})check(!Facial(name));
    const std::vector<std::string> names{"head","foot_l","hand_r","FACIAL_C_Jaw"};
    const std::vector<std::array<double,3>> pos{{0,0,180},{0,-10,0},{20,85,110},{1e12,1e12,1e12}};
    for(int angle=0;angle<360;angle+=5){const auto points=BoneDebug::Project(names,pos,angle*0.0174532925199433,400,340,28);
        check(!points[3].shown);for(int i=0;i<3;++i)check(points[i].shown&&points[i].x>=27.99&&points[i].x<=372.01&&points[i].y>=27.99&&points[i].y<=312.01);}
    check(!BoneDebug::Project(names,{},0,400,340)[0].shown);check(!BoneDebug::Project(names,pos,0,20,20)[0].shown);
    Config c=Feminine();check(Encode(Decode(Encode(c)))==Encode(c));
    {const auto split=SplitSupplement(c);check(split[0].rows.size()==3&&split[1].rows.size()==4);check(split[0].on&&split[1].on);check(split[0].rows[0].bone=="pelvis"&&split[0].rows[1].bone=="pelvis");
        for(const auto& r:split[0].rows)check(!Supplemental(r.bone));for(const auto& r:split[1].rows)check(Supplemental(r.bone));
        Config normal;normal.on=true;normal.rows.push_back({});auto alone=SplitSupplement(normal);check(alone[0].on&&!alone[1].on);normal.on=false;alone=SplitSupplement(normal);check(!alone[0].on&&!alone[1].on);check(Supplemental("PBZ_BREAST_L")&&!Supplemental("pbz_pelvis"));}
    c.rows[0].translation[0]=std::numeric_limits<double>::quiet_NaN();c.rows[0].scale[1]=-100;c.rows[0].rotation[2]=9999;c.rows.resize(100);
    c=Clean(c);check(c.rows.size()==MaxRows);check(c.rows[0].translation[0]==0&&c.rows[0].scale[1]==0.1&&c.rows[0].rotation[2]==180);
    check(Decode(std::string(16001,'x')).rows.empty());check(Decode("1,1;pelvis,1,nan").rows.empty());check(!Name("bad|name"));
    using N=NpcCombo::Node;using T=NpcCombo::NodeType;NpcCombo::Config cfg;N trigger=NpcCombo::TriggerNode(NpcCombo::Trigger::Light,0,0);trigger.next=1;
    N move;move.ability="GA_Test";N bone=NpcCombo::ModifierNode(T::BoneTransform,400,0);bone.bones=Feminine();cfg.chain={trigger,move,bone};cfg.chain[1].mods[NpcCombo::kModBone]=2;NpcCombo::EnsureKey(cfg);
    const auto encoded=NpcCombo::Encode(cfg);const auto loaded=NpcCombo::Decode(encoded);check(loaded.chain.size()==cfg.chain.size());check(loaded.chain[2].type==T::BoneTransform);check(loaded.chain[1].mods[NpcCombo::kModBone]==2);check(Encode(loaded.chain[2].bones)==Encode(bone.bones));
    std::wstring legacy=encoded;auto at=legacy.find(L"|b:");if(at!=std::wstring::npos){auto end=legacy.find(L'|',at+1);legacy.erase(at,end==std::wstring::npos?end:end-at);}
    const auto old=NpcCombo::Decode(legacy);check(old.chain.size()==cfg.chain.size()&&old.chain[2].type==T::Interval&&old.chain[2].next==-1&&old.chain[1].mods[NpcCombo::kModBone]==-1);
    for(int type=0;type<=static_cast<int>(T::BoneTransform);++type){std::vector<N> nodes(3);nodes[0].type=static_cast<T>(type);nodes[1].type=T::Speed;nodes[2].type=T::BoneTransform;check(NpcCombo::ModifierFits(nodes,0,1)==NpcCombo::ModifierFits(nodes,0,2));}
    std::vector<int> parents{-1,0,1,1};std::vector<Transform> ref(4),input(4),output;ref[1].translation[2]=100;ref[2].translation[0]=20;ref[3].translation[0]=-20;
    input[0]=ref[0];for(size_t i=1;i<4;++i)NpcGripMath::ComposeMount(ref[i],input[static_cast<size_t>(parents[i])],input[i]);
    Config static_edit;static_edit.on=true;static_edit.protect_feet=false;Row row;row.rotation[2]=90;row.translation[0]=10;static_edit.rows={row};std::vector<Binding> bindings(1);bindings[0].bone=1;
    check(Apply(static_edit,parents,ref,input,Transform{},bindings,{},1,1.0/60,output));check(std::abs(output[2].translation[0]-10)<1e-8&&std::abs(output[2].translation[1]-20)<1e-8);
    for(int i=0;i<2000;++i){check(Apply(static_edit,parents,ref,input,Transform{},bindings,{},1,1.0/60,output));check(std::abs(output[2].translation[1]-20)<1e-8);}
    static_edit.protect_feet=true;check(Apply(static_edit,parents,ref,input,Transform{},bindings,{2},1,1.0/60,output));check(output[2].translation[0]==input[2].translation[0]);
    bindings[0].bone=-1;check(!Apply(static_edit,parents,ref,input,Transform{},bindings,{},1,1.0/60,output));bindings[0].bone=1;static_edit.on=false;check(!Apply(static_edit,parents,ref,input,Transform{},bindings,{},1,1.0/60,output));
    Row spring;spring.motion=1;spring.limit=1.5;Spring state;
    {
        Row chest;chest.bone="breast_l";chest.limit=15;Row ordinary=chest;ordinary.bone="pelvis";Spring enhanced,baseline;double enhanced_peak=0,base_peak=0;
        for(int frame=0;frame<360;++frame){const std::array<double,3> target{.5*std::sin(frame/120.0*9),0,0};enhanced.Step(target,1.0/120,chest);baseline.Step(target,1.0/120,ordinary);
            enhanced_peak=(std::max)(enhanced_peak,std::abs(enhanced.offset[0]));base_peak=(std::max)(base_peak,std::abs(baseline.offset[0]));}
        check(base_peak>0.001&&std::abs(enhanced_peak/base_peak-3)<1e-8);
        Config amplified;amplified.rows={chest};amplified.rows[0].strength=5;check(Clean(amplified).rows[0].limit==15&&Clean(amplified).rows[0].strength==5);
    }
    {
        Spring duplicate;duplicate.Step({0,0,0},0,spring);duplicate.Step({1,0,0},1.0/120,spring);
        check(std::abs(duplicate.offset[0])>0.001);const auto offset=duplicate.offset,speed=duplicate.speed;
        for(int n=0;n<40;++n)duplicate.Step({1,0,0},0,spring);
        check(duplicate.offset==offset&&duplicate.speed==speed); // old zero-dt branch erased all inertia
        MotionClock clock;check(clock.Advance(1)==0);check(clock.Advance(1.0001)==0);
        check(std::abs(clock.Advance(1.008)-.008)<1e-10);check(clock.Advance(1.0081)==0);
        check(std::abs(clock.Advance(1.016)-.008)<1e-10);
        Config before;before.on=true;before.rows={spring};std::vector<Binding> prior_bindings(1);prior_bindings[0].bone=3;prior_bindings[0].spring=duplicate;
        auto edited=before;edited.rows[0].scale={2,2,2};edited.rows[0].strength=1.2;
        auto rebound=Rebind(before,prior_bindings,edited,{3});check(rebound[0].spring.offset==offset&&rebound[0].spring.speed==speed);
        Row static_row;edited.rows.insert(edited.rows.begin(),static_row);rebound=Rebind(before,prior_bindings,edited,{1,3});check(rebound[1].spring.offset==offset&&!rebound[0].spring.ready);
        edited.rows[1].on=false;check(!Rebind(before,prior_bindings,edited,{1,3})[1].spring.ready);
        check(!Rebind(before,prior_bindings,before,{4})[0].spring.ready);
    }
    {
        // Display-side four-bone fixture: live scale edits and duplicate Finalize calls.
        const std::vector<int> p{-1,0,1,1,1,1};std::vector<Transform> rest(6),pose(6),result,repeat;
        rest[1].translation[2]=90;for(size_t i=2;i<6;++i){rest[i].translation[1]=(i%2?1:-1)*10;rest[i].translation[2]=i<4?40:-5;}
        Config dynamic;dynamic.on=true;dynamic.protect_feet=false;
        for(const char* name:{"pbz_breast_l","pbz_breast_r","pbz_butt_l","pbz_butt_r"}){Row r;r.bone=name;r.motion=1;dynamic.rows.push_back(r);}
        std::vector<Binding> bound(4);for(int i=0;i<4;++i)bound[static_cast<size_t>(i)].bone=i+2;
        std::array<double,4> peak{};
        for(int frame=0;frame<360;++frame){pose[0]=rest[0];pose[0].translation[0]=8*std::sin(frame/120.0*12);
            for(size_t i=1;i<pose.size();++i)NpcGripMath::ComposeMount(rest[i],pose[static_cast<size_t>(p[i])],pose[i]);
            auto adjusted=dynamic;adjusted.rows[0].scale={1+frame*.001,1+frame*.001,1+frame*.001};bound=Rebind(dynamic,bound,adjusted,{2,3,4,5});dynamic=adjusted;
            check(Apply(dynamic,p,rest,pose,Transform{},bound,{},1,1.0/120,result));
            check(Apply(dynamic,p,rest,pose,Transform{},bound,{},1,0,repeat));
            for(size_t i=0;i<4;++i){const double delta=std::abs(result[i+2].translation[0]-pose[i+2].translation[0]);peak[i]=(std::max)(peak[i],delta);
                check(result[i+2].translation[0]==repeat[i+2].translation[0]&&delta<=dynamic.rows[i].limit+1e-8);}
        }
        for(double value:peak)check(value>.05);
    }
    for(int i=0;i<6000;++i){state.Step({std::sin(i/60.0)*20,0,100},1.0/60,spring);check(std::isfinite(state.offset[0])&&std::abs(state.offset[0])<=spring.limit);}
    state.Step({20000,0,0},1.0/60,spring);check(state.offset[0]==0&&state.speed[0]==0);state.Step({0,0,0},2,spring);check(state.offset[0]==0);
    for(int i=0;i<500;++i){auto n=cfg;n.chain[2].bones.rows.resize(MaxRows);for(size_t j=0;j<MaxRows;++j){auto& r=n.chain[2].bones.rows[j];r.bone="bone_"+std::to_string(j);r.translation[0]=(i%100)*0.01;r.rotation[2]=static_cast<double>(j);}
        const auto text=NpcCombo::Encode(n);const auto next=NpcCombo::Decode(text);check(NpcCombo::Encode(next)==text);}
    std::printf("bone_motion checks=%d failures=%d\n",checks,failed);return failed?1:0;
}
