#pragma once
#include <windows.h>
#include <cstdint>

// Force-loads Groom hair onto gameplay characters and lets the game's own
// MainHairMode logic choose between it and their mesh hair.
//  - Player: the current outfit's *_Seq hairstyle row from DT_PBZAppearanceTable,
//    applied through the native UPBZAppearanceComponent::RefreshGroomComponents.
//  - Other characters: GroomComponent templates of the matching BP_Seq_/BP_NPC_
//    blueprint, added only where the binding's target mesh exists on the actor.
// Forced Grooms get bIsMainHair and the actor's mesh hair gets
// bInfluencedByMainHairMode, so F8 switches both. Turning force off removes the
// added Grooms and restores the original flags. While on, the characters of a new
// map (or respawned ones) are forced as they appear.
//
// Outfit unlock (Shift+F6): some player blueprints (BP_Renewal_Hun_Climax) ship
// with DoPBZAppearanceLogic off, and native EquipOutfit returns at once for them.
// While on, the player's appearance component is started through the game's own
// EnableAppearanceLogicAndInit, again after a respawn or level change. Off stops
// that and clears the flag on the current pawn; its current look stays.
namespace GroomForce {
enum Command : LONG { kNone = 0, kToggle = 1, kOff = 2 };
using Logger = void (*)(const char*);
using ExternalAppearanceOwned = bool (*)(); // GT predicate; no reverse mutation.

void Initialize(Logger logger, ExternalAppearanceOwned external_owned = nullptr); // worker, before the hook starts
// GT only, bool readback; retires only the current player's forced record.
// Does not switch off the global feature or other NPC Groom records.
bool ReleasePlayerForExternal();
void OnGameThread();                     // tick hook callback
// Game thread, after OnGameThread faulted: logs where, skips the actor being forced, drops the
// donor cache; `disabled` (GameThread::kMaxFaults reached) turns Force Groom off for the session.
void OnFault(DWORD code, uintptr_t address, DWORD faults, bool disabled);
void Request(Command command);           // any thread
bool Ready();                            // hook running and layout validated
bool Active();
void Status(wchar_t* out, size_t out_size);

void RequestOutfitUnlock(bool on);       // any thread
bool OutfitUnlockActive();
void OutfitStatus(wchar_t* out, size_t out_size);

// Credits (Shift+F7): pushes the ending credits widget WB_PersonnelList onto UI.Layer.Menu
// the way the PushPersonnelList flow node does, without FinishGame, fake cycle history or a
// save. The widget rolls the credits, then switches itself to the settlement page, which can
// grant ending outfits and write the account save; it is closed on that switch.
void RequestCredits();                   // any thread
void CreditsStatus(wchar_t* out, size_t out_size);
}
