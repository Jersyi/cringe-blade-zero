#pragma once
// The hero weapons the 招式派生 page offers (combo_weapons.generated.h, read from the game's weapon tables and
// configs): the abilities each weapon grants and, for its light / heavy attacks, the native combo stage ("AAB": one
// letter per press, A light, B heavy). Casting a stage's ability starts the weapon's native combo there: the game's
// own input map (AIM_Hun_LightAttack / AIM_Hun_HeavyAttack) picks the next stage while this one is in its combo phase.
#include "combo_weapons.generated.h"
#include <string>

namespace ComboWeapons {
using ComboWeaponsData::Move;
using ComboWeaponsData::Weapon;
enum MoveKind : int { kLight = 0, kHeavy = 1, kExecution = 2, kSkill = 3 };

inline const Weapon* Find(const std::string& id) {
    for (const Weapon& weapon : ComboWeaponsData::kWeapons) if (id == weapon.id) return &weapon;
    return nullptr;
}
inline const Move* MoveOf(const std::string& id, const std::string& ability) {
    const Weapon* weapon = Find(id);
    if (!weapon) return nullptr;
    for (size_t i = 0; i < weapon->count; ++i) if (ability == weapon->moves[i].ability) return &weapon->moves[i];
    return nullptr;
}
inline bool IsShaqi(const std::string& id) { const Weapon* weapon = Find(id); return weapon && weapon->shaqi; }
inline bool HeldAttack(const std::string& ability) {
    return ability.find("_B_Hold")!=std::string::npos || ability.find("_BCharge")!=std::string::npos;
}
// "AAB" -> "轻轻重"; empty for a move outside the numbered stages.
inline std::wstring StagePresses(const char* stage) {
    std::wstring out;
    for (const char* c = stage; c && *c; ++c) out += *c == 'B' ? L'重' : L'轻';
    return out;
}
}
