#include "air_attack_reset.h"
#include <array>
#include <cstdio>
#include <cwchar>

int main() {
    using namespace AirSkills;
    int failures=0,checks=0;
    const auto check=[&](bool ok,const char* what) { ++checks; if (!ok) { ++failures; std::printf("FAIL %s\n",what); } };
    for (const auto initial: {std::array<int32_t,3>{0,0,0},{1,0,0},{1,1,1},{2,3,2},{32,32,32}}) {
        auto counts=initial; int removes=0;
        auto read=[&](size_t i,int32_t& out) { out=counts[i]; return true; };
        auto remove=[&](size_t i) { --counts[i]; ++removes; return true; };
        const auto result=ResetAirAttackStages(read,remove);
        check(result.verified&&counts==std::array<int32_t,3>{0,0,0},"consumed stages clear, including stacked counts");
        check(removes==initial[0]+initial[1]+initial[2],"no remove for an absent stage");
        check(result.before[0]==initial[0]&&result.before[1]==initial[1]&&result.before[2]==initial[2],"receipt retains original counts");
        const auto again=ResetAirAttackStages(read,remove);
        check(again.verified&&removes==initial[0]+initial[1]+initial[2],"repeating an empty reset has no writes");
    }
    for (int32_t bad: {-1,33}) {
        int writes=0;
        const auto result=ResetAirAttackStages([&](size_t i,int32_t& out) { out=i==2?bad:1; return true; },[&](size_t) { ++writes; return true; });
        check(!result.verified&&writes==0,"invalid count rejects all writes during preflight");
    }
    {
        int writes=0;
        const auto result=ResetAirAttackStages([](size_t i,int32_t& out) { out=1; return i!=2; },[&](size_t) { ++writes; return true; });
        check(!result.verified&&writes==0,"missing query or owner rejects writes");
    }
    {
        int writes=0;
        const auto result=ResetAirAttackStages([](size_t,int32_t& out) { out=1; return true; },[&](size_t) { ++writes; return true; });
        check(!result.verified&&writes==1,"unchanged GE-owned count stops without spinning");
    }
    {
        int writes=0;
        const auto result=ResetAirAttackStages([](size_t,int32_t& out) { out=1; return true; },[&](size_t) { ++writes; return false; });
        check(!result.verified&&writes==1,"failed native remove is not reported successful");
    }
    {
        int writes=0;
        const auto result=ResetAirAttackStages([&](size_t,int32_t& out) { out=1; return writes==0; },[&](size_t) { ++writes; return true; });
        check(!result.verified&&writes==1&&result.after[2]==-1,"lost owner after remove stops and reports unknown readback");
    }
    {
        std::array<int32_t,3> counts{1,1,1};
        const auto result=ResetAirAttackStages([&](size_t i,int32_t& out) { out=counts[i]; return true; },[&](size_t i) { --counts[i]; if (i==0) counts[2]=1; return true; });
        check(!result.verified,"final read detects a stage re-added by a native callback");
    }
    check(std::wcscmp(kAirAttackTags[0],L"CharacterState.AirState.AirAtk1")==0&&
          std::wcscmp(kAirAttackTags[1],L"CharacterState.AirState.AirAtk2")==0&&
          std::wcscmp(kAirAttackTags[2],L"CharacterState.AirState.AirAtk3")==0,"only the three consumed attack stages are targeted");
    std::printf("Air attack reset: checks=%d failures=%d (mocked tag API; no game runtime)\n",checks,failures);
    return failures?1:0;
}
