// An effect action's weapon lookup on an NPC skin (ExecWeaponLookup in npc_retarget.cpp), the array rule alone so it can
// be tested without the game. items[0..num) is the answer of WeaponUtils.GetWeaponActorsByDisplayIndex; stand_in(item)
// gives the display index of a stand-in actor (AddStandIns) or -1. The weapon effect actions cast what they get to
// SocketComponentHolder, which a stand-in is not: each stand-in becomes host (the hero weapon of index 0), host listed
// once. Nothing is touched unless a stand-in or host is listed (another avatar's lookup).
#pragma once
#include <cstdint>

namespace NpcFxLookup {
struct Result {
    bool related = false;  // a stand-in or host was listed: the hero's own lookup
    int32_t row = -1;      // the first stand-in's display index (-1: none)
    bool only = false;     // nothing but stand-ins was listed: what spawns next belongs to boss weapon row `row`
    int32_t num = 0;       // the array's new length
};
template<class T, class F> Result Rewrite(T* items, int32_t num, T host, F stand_in) {
    Result r; r.num = num;
    int32_t others = 0;
    for (int32_t i = 0; i < num; ++i) {
        const int32_t index = stand_in(items[i]);
        if (index >= 0) { if (r.row < 0) r.row = index; r.related = true; }
        else { ++others; r.related = r.related || items[i] == host; }
    }
    if (!r.related || r.row < 0 || !host) return r;
    int32_t kept = 0; bool has_host = false;
    for (int32_t i = 0; i < num; ++i) {
        const T item = stand_in(items[i]) >= 0 ? host : items[i];
        if (item == host) { if (has_host) continue; has_host = true; }
        items[kept++] = item;
    }
    r.num = kept; r.only = others == 0;
    return r;
}
}  // namespace NpcFxLookup
