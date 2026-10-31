#pragma once
#include <cstdint>
#include <cstddef>
#include <string>

// Grab add-ons around a paired MotianGrab session (game thread for Begin/Tick/
// Finish; options from the worker). Weapon hiding keeps the player's weapon
// actors hidden for the whole grab and restores only what it hid; the source
// Boss's weapon mesh is shown in their place (a display-only component) and
// follows the grab's own armed / force-hide state. Damage is
// applied after the native grab ends, through the game's own attribute call
// (AttributeUtils.ModifyCharacterAttributeValue), never by raw memory writes.
namespace GrabExtras {
enum class Damage : int { Native = 0, Multiplier = 1, Percent = 2 };
struct Options {
    bool hide_weapon = true;
    Damage damage = Damage::Native;
    float multiplier = 2.0f;   // x native grab damage, 0.1 to 10
    float percent = 30.0f;     // % of the target's max health, 1 to 100
    // When the target took no native damage (a boss victim usually takes none,
    // or it is invulnerable) the move's last observed native damage ([GrabDamage]
    // in the ini), else 6 % of max health, is the base the mode scales. Native
    // mode applies that base only with ignore_invulnerable, which also lets the
    // grab target a hit-reacting / special-move / invulnerable boss.
    bool ignore_invulnerable = false;
};
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
// Boss's own performance (proxy_scale maps the Boss's height to the player's).
struct Weapon {
    unsigned char* actor_class = nullptr; const char* class_path = nullptr; const char* socket = nullptr; const double* rest = nullptr;
    unsigned char* proxy_mesh = nullptr; double proxy_scale = 1;
};
constexpr size_t kMaxWeapons = 4; // both hands, two pieces each (e.g. forearm guards)
// Game thread.
// move: the grab move id, used to remember its native damage.
void Begin(unsigned char* pawn, unsigned char* target, const Weapon* weapons = nullptr, size_t weapon_count = 0, const char* move = nullptr);
void Tick();
void Finish(bool completed);
bool Active();
// The executor's own grab ended: grab damage is measured and applied now, so
// hits landed while the target is still getting up are never scaled.
void SourceEnded();
}
