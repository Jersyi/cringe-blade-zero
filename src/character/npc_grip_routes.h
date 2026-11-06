#pragma once
// Which things hanging on the hero's mesh become weapon routes (the grip adapter maps a routed socket onto the NPC
// body's hand or bone), and which discovered routes go again. Pure rules, so the unit tests can check them; the
// runner (RefreshWeaponRoutes) reads the facts off the live components.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace NpcGripRoutes {

// One component attached to the hero's source mesh.
struct Attached {
    uint64_t socket = 0;          // its AttachSocketName as an FName key (0: none)
    bool owner_alive = false;     // GetOwner gave a live actor
    bool owner_is_pawn = false;   // the hero's own part (collision boxes, its own effects)
    bool owner_is_world = false;  // owned by the WorldSettings actor: the engine's effect pools (Niagara / Cascade)
    bool effect = false;          // an effect component (FXSystemComponent: Niagara, Cascade)
};

// Things that are weapons: another actor's components that are not effects. Pooled effects (a footstep or landing
// burst on foot_r_Socket, a dash trail on Root) are owned by WorldSettings, come and go every few frames and must
// never remap a socket the game itself reads (feet, root, interaction).
inline bool Weapon(const Attached& a) { return a.owner_alive && !a.owner_is_pawn && !a.owner_is_world && !a.effect; }

// May the socket of this attachment become a route now: a weapon, on a named socket the mesh has, not routed yet.
inline bool TakesRoute(const Attached& a, bool socket_exists, bool routed) { return Weapon(a) && a.socket && socket_exists && !routed; }

// A route RefreshWeaponRoutes added (`found`) goes again once no weapon hangs on its socket; the generated mounts
// stay. Returns the indexes of `routes` to drop, highest first (so erasing them in order keeps the rest in place).
struct Route { uint64_t socket = 0; bool found = false; };
inline std::vector<size_t> StaleRoutes(const std::vector<Route>& routes, const std::vector<Attached>& attached) {
    std::vector<size_t> stale;
    for (size_t i = routes.size(); i-- > 0;) {
        if (!routes[i].found) continue;
        bool held = false;
        for (const Attached& a : attached) if (a.socket == routes[i].socket && Weapon(a)) { held = true; break; }
        if (!held) stale.push_back(i);
    }
    return stale;
}

}  // namespace NpcGripRoutes
