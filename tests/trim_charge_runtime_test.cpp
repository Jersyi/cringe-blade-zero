#include "trim_charge_runtime.h"
#include <cstdio>
namespace {
alignas(16) unsigned char actor[128]{},anim[128]{},montage[128]{};
bool actor_alive=true,anim_alive=true,paused=false,refuse=false;unsigned resumes=0;
}
namespace ue {
bool Alive(Object* o){return (o==actor&&actor_alive)||(o==anim&&anim_alive)||o==montage;}
Name NameOf(Object* o){return {o==actor?1u:o==anim?2u:3u,0};}
Object* ClassOf(Object* o){return o;}
int32_t Offset(Object*,const char* name){return !std::strcmp(name,"CustomTimeDilation")?64:-1;}
Call::Call(Object* target,const char* name):target_(target){function_=!std::strcmp(name,"Montage_Pause")?actor:montage;}
unsigned char* Call::Slot(const char*,size_t bytes){return bytes<=sizeof(params_)?params_:nullptr;}
bool Call::Invoke(){if(refuse||!Alive(target_))return false;if(function_==actor)paused=true;else{paused=false;++resumes;}return true;}
}
int main(){int checks=0,failures=0;const auto check=[&](bool ok){++checks;if(!ok){++failures;std::printf("FAIL %d\n",checks);}};
    TrimChargeRuntime::Pause lease;ue::At<float>(actor,64)=2.5f;
    check(lease.Begin(actor,anim,montage)&&paused&&ue::At<float>(actor,64)==0);
    for(int i=0;i<500;++i)check(lease.Hold()&&ue::At<float>(actor,64)==0);
    lease.Release();check(!paused&&!lease.active&&ue::At<float>(actor,64)==2.5f&&resumes==1);
    lease.Release();check(resumes==1);
    check(lease.Begin(actor,anim,montage));ue::At<float>(actor,64)=1.3f;
    check(lease.Hold()&&lease.before==1.3f);lease.Reset();check(!paused&&ue::At<float>(actor,64)==1.3f&&!lease.active);
    refuse=true;check(!lease.Begin(actor,anim,montage)&&ue::At<float>(actor,64)==1.3f);refuse=false;
    check(lease.Begin(actor,anim,montage));actor_alive=false;check(!lease.Hold()&&!lease.active&&!paused);actor_alive=true;
    ue::At<float>(actor,64)=1;check(lease.Begin(actor,anim,montage));anim_alive=false;lease.Reset();check(ue::At<float>(actor,64)==1&&!lease.active);anim_alive=true;paused=false;
    check(lease.Begin(actor,anim,montage));ue::At<float>(actor,64)=.7f;lease.Release();check(ue::At<float>(actor,64)==.7f);
    std::printf("trim_charge_runtime checks=%d failures=%d runtime=false\n",checks,failures);return failures?1:0;
}
