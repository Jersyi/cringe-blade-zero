#pragma once
#include <cstddef>
#include <cstdint>

namespace AirSkills {
// Cooked BP_Air_A/AA/AAA bases add these loose tags until landing. Reset only
// these consumed attack stages, never the AirState parent or movement state.
inline constexpr const wchar_t* kAirAttackTags[] = {
    L"CharacterState.AirState.AirAtk1",
    L"CharacterState.AirState.AirAtk2",
    L"CharacterState.AirState.AirAtk3",
};
struct AirAttackReset {
    int32_t before[3]{-1,-1,-1}, after[3]{-1,-1,-1};
    bool verified=false;
};
// Read/remove are game-thread adapters. Read validates owner identity and the
// reflected call; remove subtracts one loose count through the game's API.
// Bound work and stop on an unexpected count (e.g. a GE-owned tag or callback).
template<class Read, class Remove>
AirAttackReset ResetAirAttackStages(Read&& read, Remove&& remove) {
    AirAttackReset result;
    for (size_t i=0;i<3;++i) {
        if (!read(i,result.before[i]) || result.before[i]<0 || result.before[i]>32) return result;
        result.after[i]=result.before[i];
    }
    for (size_t end=3;end>0;--end) {
        const size_t i=end-1;
        while (result.after[i]>0) {
            const int32_t expected=result.after[i]-1;
            if (!remove(i)) return result;
            result.after[i]=-1;
            int32_t observed=-1;
            if (!read(i,observed)) return result;
            result.after[i]=observed;
            if (observed!=expected) return result;
        }
    }
    // Removing a later tag can notify native listeners. Verify all three again.
    for (size_t i=0;i<3;++i) {
        result.after[i]=-1;
        int32_t observed=-1;
        if (!read(i,observed)) return result;
        result.after[i]=observed;
        if (observed!=0) return result;
    }
    result.verified=true;
    return result;
}
}
