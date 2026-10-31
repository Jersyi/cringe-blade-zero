#pragma once
#include "ue_runtime.h"

// Game-thread helpers for a safe encounter entry. No worker/UI may invoke the
// engine-facing methods. HasHold/IsSafe read only the published plain state.
namespace EntrySafety {
using Logger = void (*)(const char*);
struct alignas(16) Transform {
    double rotation[4]{0,0,0,1}, translation[4]{}, scale[4]{1,1,1,0};
};
static_assert(sizeof(Transform) == 96, "PBZ native FTransform layout");
void Initialize(Logger logger);
bool HoldPawn(ue::Object* pawn);
Transform ParkingTransform(const Transform& raw);
bool PlacePlayer(ue::Object* pawn, ue::Object* game_instance, const Transform& raw_boss_anchor, bool return_to_original = false);
bool MakeBossTransform(ue::Object* character_class, const Transform& raw, Transform& result);
bool ReleasePawn();
// Unsafe old pawns stay MOVE_None; retain their baseline until GC or reclaim.
bool BeforeTravel();
bool HasHold();
bool IsSafe();
}
