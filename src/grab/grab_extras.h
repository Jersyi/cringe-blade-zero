#pragma once
#include <cstdint>
#include <cstddef>
#include <string>

// Grab add-ons around a paired MotianGrab session (game thread for Begin/Tick/
// Finish; options from the worker). Weapon hiding keeps the player's weapon
// actors hidden for the whole grab and restores only what it hid; the source
// Boss's weapon mesh is shown in their place (a display-only component) and
// follows the grab's own armed / force-hide state. Damage scales each of the
// grab's own damage events as it lands, through the game's own attribute call
// (AttributeUtils.ModifyCharacterAttributeValue).
namespace GrabExtras {
enum class Damage : int { Native = 0, Multiplier = 1, Percent = 2 };
constexpr float kMaxMultiplier = 30.0f;
constexpr float kMultiplierPresets[] = {4.5f, 10.0f, 15.0f, 20.0f, 30.0f};
struct Options {
    bool hide_weapon = true;
    Damage damage = Damage::Native;
    float multiplier = 2.0f;   // x each native grab hit, 0.1 to kMaxMultiplier
    float percent = 30.0f;     // each native grab hit deals at least this % of the target's max health, 1 to 100
    // Only with ignore_invulnerable: a target that took no damage event at all (a boss
    // victim usually takes none, or it is invulnerable) loses the move's last observed
    // native damage ([GrabDamage] in the ini), else 6 % of max health, scaled by the
    // mode, once when the grab ends; a change its attributes refuse is written
    // directly (never below 1). It also lets the grab target a hit-reacting /
    // special-move / invulnerable boss.
    bool ignore_invulnerable = false;
};
// Extra health the target loses for one of the grab's own damage events (a native hit of
// drop): Multiplier makes the hit deal drop x multiplier (below 1 gives some back), Percent
// makes it deal at least percent of maximum, Native adds nothing. Callers cap it at the
// health left.
inline float HitExtra(Damage mode, float drop, float multiplier, float percent, float maximum) {
    if (!(drop > 0)) return 0;
    if (mode == Damage::Multiplier) {
        const float m = multiplier < .1f ? .1f : multiplier > kMaxMultiplier ? kMaxMultiplier : multiplier;
        return drop * (m - 1);
    }
    if (mode == Damage::Percent && maximum > 0) {
        const float p = percent < 1 ? 1 : percent > 100 ? 100 : percent, floor = maximum * p / 100;
        return floor > drop ? floor - drop : 0;
    }
    return 0;
}
using Logger = void (*)(const char*);
void Initialize(Logger logger); // Loads persisted options.
void SetOptions(const Options& options); // Worker/UI; persisted.
Options GetOptions();
std::wstring LastResult();
// One source-Boss weapon to show in the player's hand during the grab: the
// loaded (pinned) weapon actor class and its config attach socket.
// rest: the source skeleton's weapon bone under its hand (quat xyzw, translation), or null.
// proxy_mesh: a loaded skeletal mesh on the source Boss's skeleton; an invisible
// copy replays the player's grab montage so the weapon moves exactly as in the
// Boss's own performance (proxy_scale maps the Boss's height to the hero's; with an
// NPC skin it is further scaled by the skin's size, and the weapon with it).
struct Weapon {
    unsigned char* actor_class = nullptr; const char* class_path = nullptr; const char* socket = nullptr; const double* rest = nullptr;
    unsigned char* proxy_mesh = nullptr; double proxy_scale = 1;
};
constexpr size_t kMaxWeapons = 4; // both hands, two pieces each (e.g. forearm guards)
// Game thread.
// move: the grab move id, used to remember its native damage.
// hero_over_boss: the hero's height over the executing boss's (kGrabScales). The boss weapon is
// scaled to the current model, skin size * hero_over_boss, and follows its hand; the models and the
// victim keep their own size and the place the paired timeline gives them.
void Begin(unsigned char* pawn, unsigned char* target, const Weapon* weapons = nullptr, size_t weapon_count = 0, const char* move = nullptr, double hero_over_boss = 1);
void Tick();
void Finish(bool completed);
bool Active();
// True from Begin while the player still plays the grab's own montage; false once another montage
// (or none) takes over, even if the grab ability has not ended yet.
bool Posing();
// Game thread: the mesh the player is drawn with when it is not the pawn's own (an NPC skin), else
// null, and size: that model's height over the hero's (1 without a skin). Each boss weapon is moved
// every tick by that mesh's hand minus the hand it hangs on.
using SkinDisplay = unsigned char* (*)(double& size);
void SetSkinDisplay(SkinDisplay provider);
// The executor's own grab ended: hits stop being scaled (hits landed while the
// target is still getting up never are) and the outcome is reported.
void SourceEnded();
// Game thread: an additive change of a character's Health through the same game attribute call the grab
// damage uses (AttributeUtils.ModifyCharacterAttributeValue; negative removes health), so death and
// hit callbacks stay the game's own. False when the call or the attribute layout is unavailable.
bool ChangeHealth(unsigned char* character, float delta);
// Game thread. delta added to the maximum health attribute (Health_Max, else MaxHealth); the runtime audit's dummy.
bool ChangeMaxHealth(unsigned char* character, float delta);
}
