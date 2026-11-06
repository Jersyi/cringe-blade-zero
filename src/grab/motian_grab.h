#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace MotianGrab {
using Logger = void (*)(const char*);
using InputFocus = bool (*)();
struct Options {
    bool enabled = false;
    bool allow_bosses = true; // Humanoid bosses still pass the same physical/interface/tag checks.
    bool position_correction = true; // Authored pair alignment; never a per-frame teleport.
    int32_t variant = 0; // Global move index. Original MoTian moves remain 0..3.
    float range = 280.0f; // Capsule-surface distance; clamped to the selected move's range.
    // 黑伤 (Tang) executions play MoTian's full grab instead: the native execution is
    // cancelled as it starts and that move runs on its target. Independent of `enabled`.
    bool tang_execution = true;
    // 投技时变身: a grab the hero starts (grab key, 投技 page) first takes the look of the move's boss (an NPC skin),
    // and the look from before comes back when the grab has ended. Grabs of the move chain (招式派生) never do.
    bool transform = false;
};
struct Variant {
    std::wstring id, name, detail;
    int32_t index = -1; // Submit this value after filtering; never the UI row.
    std::wstring boss_id, boss_name;
    std::wstring source; // the executor's grab ability (class path)
    bool supported = false;
    std::wstring reason;
    float max_range = 400.0f, recommended_range = 280.0f;
    float authored_distance = 0.0f; // the boss move's own first-frame pair distance (cm)
};
struct BossInfo {
    std::wstring id, name, status;
    uint32_t move_count = 0;
    bool supported = false;
};
struct Snapshot {
    Options options;
    bool ready = false, active = false, paired = false, cleanup_pending = false, disabled = false;
    // A chain grab cut away from the hero (RequestCut): its executor's side ended and settled, its target's side still
    // holds the target; holding_token names that cut (0: none).
    bool holding = false; uint32_t holding_token = 0;
    // The hero's side of the running grab ended and settled (its displays gone): only its target's side, or the
    // target's return to its baseline, is still running. The hero is free (a chain's 投技 node is over).
    bool hero_free = false;
    uint32_t attempts = 0, activations = 0, paired_hits = 0;
    uint32_t retained_classes = 0;
    bool prepared = false, preparing = false;
    uint32_t prepared_asset_refs = 0;
    float preparation_last_ms = 0;
    std::wstring preparation_status;
    bool position_corrected = false;
    std::wstring alignment_status;
    bool execution_prepared = false; // MoTian's full grab is loaded for 黑伤 executions.
    uint32_t executions_redirected = 0;
    std::wstring execution_status;
    // The move chain's own grab (招式派生 投技 nodes): the move it keeps loaded (-1 none), whether that is ready.
    int32_t chain_variant = -1;
    bool chain_prepared = false, chain_preparing = false;
    std::wstring chain_status;
    std::wstring target, ability;
    std::wstring status = L"投技默认关闭；选择来源 Boss 的招式，开启后按投技快捷键触发一次";
    std::vector<Variant> variants;
    std::vector<BossInfo> bosses;
};
// Worker/UI only post options/requests. The callback must be side-effect-free;
// null denies gameplay input. Every UObject/native operation runs on GT.
void Initialize(Logger logger, InputFocus can_gameplay_input = nullptr);
bool RequestOptions(const Options& options);
bool RequestTrigger();
bool RequestTrigger(int32_t expected_global_index); // GT rejects an unapplied UI selection.
// transform false: never 投技时变身 (the move chain and the audit keep the look they run in).
bool RequestTrigger(int32_t expected_global_index, bool transform);
// The move chain's grab slot: `variant` is kept loaded beside the 投技 page's move (-1 releases it). It needs no 开启
// and never changes the page's move.
bool RequestChainMove(int32_t variant);
// The chain's grab of `variant` now, from that slot: no 开启 needed, no 投技时变身, humanoid bosses allowed, the move's
// own full range; the focus, health, ground and target gates still hold. Tried for about a second, then counted as an
// attempt that did not start.
bool RequestChainTrigger(int32_t variant);
void RequestCancel();
// The move chain cut its paired grab (投技 under 允许中断): only the executor's side is cancelled (and settled once it
// has ended: cleanup proof, state lease, its grants cleared); the target's side keeps running, so the target stays in
// its grabbed animation, held by the game's own ability, while the chain's boss body plays the grab out. `token` names
// the cut. RequestRelease(token) ends the target's side of that cut (a stale token does nothing); the target's side
// ending by itself ends it too. A grab that cannot be held (not paired, a QTE grab) is cancelled as by RequestCancel.
void RequestCut(uint32_t token);
void RequestRelease(uint32_t token);
Snapshot GetSnapshot();
void OnGameThreadGuarded();
// Cancel/verify only this module's exact grant handles before map/pawn changes.
// False must block travel; selected options are retained.
bool BeforeTravel();
bool HandleCommand(const char* verb, const char* arguments);
}
