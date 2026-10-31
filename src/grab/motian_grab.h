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
};
struct Variant {
    std::wstring id, name, detail;
    int32_t index = -1; // Submit this value after filtering; never the UI row.
    std::wstring boss_id, boss_name;
    bool supported = false;
    std::wstring reason;
    float max_range = 400.0f, recommended_range = 280.0f;
};
struct BossInfo {
    std::wstring id, name, status;
    uint32_t move_count = 0;
    bool supported = false;
};
struct Snapshot {
    Options options;
    bool ready = false, active = false, paired = false, cleanup_pending = false, disabled = false;
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
    std::wstring target, ability;
    std::wstring status = L"投技默认关闭；选择来源 Boss 的招式，开启后 PS5 三角 + 圈触发一次";
    std::vector<Variant> variants;
    std::vector<BossInfo> bosses;
};
// Worker/UI only post options/requests. The callback must be side-effect-free;
// null denies gameplay input. Every UObject/native operation runs on GT.
void Initialize(Logger logger, InputFocus can_gameplay_input = nullptr);
bool RequestOptions(const Options& options);
bool RequestTrigger();
bool RequestTrigger(int32_t expected_global_index); // GT rejects an unapplied UI selection.
void RequestCancel();
Snapshot GetSnapshot();
void OnGameThreadGuarded();
// Cancel/verify only this module's exact grant handles before map/pawn changes.
// False must block travel; selected options are retained.
bool BeforeTravel();
bool HandleCommand(const char* verb, const char* arguments);
}
