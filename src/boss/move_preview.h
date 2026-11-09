#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include "npc_combo_model.h"

// 招式预览 (招式派生 page): a mouse resting on a move shows it played by its boss on a stage of its own. The stage is
// built when first needed, kStageLiftCm above the hero, from the game's cooked engine assets: a WorldGrid floor, a grey
// sky sphere around it (it hides the level), a wooden dummy and two lights. A scene capture renders it every frame into
// a render target, and the game itself draws that target live (a UMG image in its viewport) where the page's card is
// (Place): the page leaves a hole in the hub there (Frame::live). Only when that widget cannot be made is the capture
// read back a few times a second and handed to the page as BGRA pixels. The boss is BossAssist's preview boss: the move
// again and again, back on its spot between casts. While the hub pauses the game (HubPause), the world runs for the
// preview (NeedsWorld) and the hero, the enemies and their controllers stand still instead. Nothing of the stage is in
// the player's view; its sounds are too far away to be heard. Hidden for kStageKeepMs: all removed.
namespace MovePreview {
using Logger = void (*)(const char*);
constexpr int kWidth = 320, kHeight = 180;
void Initialize(Logger logger);
// Any thread. monster / ability: package paths (kNpcSkillSets); Show again with the same pair keeps it going (a new
// speed takes effect at once). speed: the move's 加速 (percent); curve: its 曲线调节 (2+ points: the speed over each cast).
// trim_from / trim_to: its 裁剪 (ms of its montage; 0 / 0: all of it), updated on the current cast (timeline + montage seek).
void Show(const std::string& monster, const std::string& ability, int speed = 100, const std::vector<NpcCombo::CurvePoint>& curve = {}, int trim_from = 0, int trim_to = 0);
void Hide();
// Any thread: the newest frame (kWidth x kHeight BGRA, top row first) when its serial is newer than `serial`; status:
// what the stage does now (empty while frames come).
// live: the game draws the stage at the Place rect itself; no pixels come.
struct Frame { std::vector<uint32_t> pixels; uint64_t serial = 0; std::wstring status; bool live = false; };
bool Latest(Frame& frame, uint64_t newer_than);
void OnGameThreadGuarded();
// Any thread: where the card's picture is, in the game window's client pixels (empty: nowhere; the live view goes).
void Place(const RECT& client);
// Game thread: a preview is up (HubPause keeps the world running for it).
bool NeedsWorld();
// Game thread: the hero and the enemies are held still for the preview (NpcRetarget's slow-motion pass stays out).
bool HoldsGameplay();
// Tests (npcretarget preview source <n> | cull <0/1> | fx): the capture's source (ESceneCaptureSource), whether Niagara
// keeps culling while the boss plays, one logged scan of the effects near the stage.
void Debug(const std::string& what, int value);
}
