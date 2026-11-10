#pragma once
#include "ue_runtime.h"
#include <windows.h>
#include <cstdint>
#include <cmath>

// Per-cast actor time lease, using the same native dilation field as the preview
// freeze. No shared animation assets or ability defaults are changed.
namespace TrimChargeRuntime {
struct Pause {
    ue::Object* actor=nullptr;ue::Object* anim=nullptr;ue::Object* montage=nullptr;
    ue::Name actor_name{},anim_name{},montage_name{};int offset=-1;
    float before=1;bool active=false;uint64_t since=0,total=0;
    static bool Same(ue::Object* o,ue::Name n){return ue::Alive(o)&&ue::NameOf(o)==n;}
    uint64_t Millis()const{return total+(active?GetTickCount64()-since:0);}
    bool Begin(ue::Object* pawn,ue::Object* instance,ue::Object* clip){
        if(active)return true;
        if(!ue::Alive(pawn)||!ue::Alive(instance)||!ue::Alive(clip))return false;
        const int field=ue::Offset(ue::ClassOf(pawn),"CustomTimeDilation");if(field<0)return false;
        const float value=ue::At<float>(pawn,field);if(!std::isfinite(value)||value<0)return false;
        if(!ue::Call(instance,"Montage_Pause").Set("Montage",clip).Invoke())return false;
        actor=pawn;anim=instance;montage=clip;actor_name=ue::NameOf(pawn);anim_name=ue::NameOf(instance);montage_name=ue::NameOf(clip);
        offset=field;before=value;since=GetTickCount64();active=true;ue::At<float>(actor,offset)=0;return true;
    }
    bool Hold(){
        if(!active||!Same(actor,actor_name)||!Same(anim,anim_name)||!Same(montage,montage_name)){Release();return false;}
        float& current=ue::At<float>(actor,offset);if(current!=0&&std::isfinite(current)&&current>0)before=current;
        current=0;return true;
    }
    void Release(){
        if(!active)return;
        total+=GetTickCount64()-since;active=false;
        if(Same(actor,actor_name)&&offset>=0&&ue::At<float>(actor,offset)==0)ue::At<float>(actor,offset)=before;
        if(Same(anim,anim_name)&&Same(montage,montage_name))ue::Call(anim,"Montage_Resume").Set("Montage",montage).Invoke();
    }
    void Reset(){Release();*this={};}
};
}
