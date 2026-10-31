#include "boss_ai.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <iterator>

namespace BossAI {
namespace {
using ue::Object;
using ue::Name;
using ue::Call;
struct KnownFloat { const wchar_t* key; Kind kind; float maximum; };
constexpr KnownFloat kKnown[] = {
    {L"中距离进攻概率", Kind::Probability, 1},
    {L"远距离进攻概率", Kind::Probability, 1},
    {L"事件反应-打差合概率", Kind::Probability, 1},
    {L"玩家开始近战攻击-格挡权重", Kind::Weight, 10000},
    {L"玩家开始近战攻击-无行为权重", Kind::Weight, 10000},
    {L"远程攻击发射-格挡权重", Kind::Weight, 10000},
    {L"远程攻击发射-无行为权重", Kind::Weight, 10000},
    {L"立回-近距离闪避权重", Kind::Weight, 10000},
    {L"立回-中距离闪避权重", Kind::Weight, 10000},
    {L"autocombo权重", Kind::Weight, 10000},
    {L"非autocombo权重", Kind::Weight, 10000},
    {L"套路权重", Kind::Weight, 10000},
    {L"非套路权重", Kind::Weight, 10000},
};
// Verified const getter: uses instance ParameterCollection, or rebuilds and
// returns MergedParameterCollection when its instance override flag is true.
constexpr uintptr_t kGetParameters = 0x05d00760;
constexpr unsigned char kGetParametersCode[] = {
    0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x81,
    0xec, 0x80, 0x0a, 0x00, 0x00, 0x48, 0x8b, 0x05
};
struct Row {
    Parameter ui;
    Name name;
    int32_t source_collection = -1, cooldown_index = -1;
};
struct Cooldown {
    Name name;
    float original_definition = 0, original_remaining = 0, clock = 0;
    bool changed = false;
};
enum class Action { Set, Refresh, Restore };
struct Request { uint64_t generation; Action action; int32_t index; float value; };
SRWLOCK state_lock = SRWLOCK_INIT, queue_lock = SRWLOCK_INIT;
Snapshot state_;
std::deque<Request> queue_;
std::vector<Row> rows_;
std::vector<Cooldown> cooldowns_;
Logger logger_ = nullptr;
Object* pawn_ = nullptr, *component_ = nullptr, *cooldown_ = nullptr;
Name pawn_name_, component_name_, cooldown_name_;
uint64_t generation_ = 0;
int32_t normal_offset_ = -1, override_offset_ = -1, override_flag_ = -1, float_offset_ = -1;

void Log(const char* text) { if (logger_) logger_(text); }
bool Same(Object* object, Name name) { return object && ue::Alive(object) && ue::NameOf(object) == name; }
bool Readable(const void* pointer, size_t bytes) {
    MEMORY_BASIC_INFORMATION memory{};
    const uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
    if (!pointer || !VirtualQuery(pointer, &memory, sizeof(memory)) || memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    const uintptr_t region = reinterpret_cast<uintptr_t>(memory.BaseAddress);
    return address >= region && bytes <= memory.RegionSize && address - region <= memory.RegionSize - bytes;
}
bool Near(float a, float b, float tolerance = 0.001f) { return std::isfinite(a) && std::isfinite(b) && std::fabs(a - b) <= tolerance; }
Object* Property(Object* object, const char* property) {
    if (!object || !ue::Alive(object)) return nullptr;
    const int32_t offset = ue::Offset(ue::ClassOf(object), property);
    return offset >= 0 ? ue::At<Object*>(object, offset) : nullptr;
}
std::wstring WideName(Name name) {
    const uintptr_t pool = ue::Base() + ue::layout::kNamePool;
    const uint32_t current = ue::At<uint32_t>(reinterpret_cast<void*>(pool), 8), block = name.index >> 16;
    if (block > current || block >= 8192) return {};
    const uintptr_t entry = ue::At<uintptr_t>(reinterpret_cast<void*>(pool + 16), static_cast<int32_t>(block * 8)) + (name.index & 65535u) * 2u;
    const uint16_t header = ue::At<uint16_t>(reinterpret_cast<void*>(entry), 0);
    const uint16_t length = header >> 6;
    if (!length || length >= 1024) return {};
    std::wstring text;
    text.reserve(length + 12);
    if (header & 1) text.assign(reinterpret_cast<const wchar_t*>(entry + 2), length);
    else for (uint16_t i = 0; i < length; ++i) text.push_back(static_cast<wchar_t>(ue::At<uint8_t>(reinterpret_cast<void*>(entry + 2), i)));
    if (name.number) text += L"_" + std::to_wstring(name.number - 1);
    return text;
}

// This exact build's TMap<FName,float>: 20-byte sparse elements, value@8.
// Native getter 0x5d49a00 uses index*20 and +8, not a guessed packed map.
// The allocation bitmap is checked so free sparse entries are never exposed.
template<class Visitor> bool VisitMap(void* map, size_t stride, Visitor&& visitor) {
    if (!Readable(map, 80)) return false;
    const int32_t count = ue::At<int32_t>(map, 8), capacity = ue::At<int32_t>(map, 12),
        bits = ue::At<int32_t>(map, 40), max_bits = ue::At<int32_t>(map, 44), free = ue::At<int32_t>(map, 52);
    if (count < 0 || count > 4096 || capacity < count || bits != count || max_bits < bits || free < 0 || free > count) return false;
    if (!count) return true;
    auto* data = ue::At<unsigned char*>(map, 0);
    auto* allocation = ue::At<uint32_t*>(map, 32);
    if (!allocation) allocation = reinterpret_cast<uint32_t*>(static_cast<unsigned char*>(map) + 16);
    if (!Readable(data, static_cast<size_t>(count) * stride) || !Readable(allocation, (static_cast<size_t>(bits) + 31) / 32 * 4)) return false;
    int32_t seen = 0;
    for (int32_t i = 0; i < count; ++i) if (allocation[i / 32] & (1u << (i % 32))) {
        ++seen;
        if (!visitor(data + static_cast<size_t>(i) * stride)) return true;
    }
    return seen == count - free;
}
float* FindFloat(void* map, Name key) {
    float* found = nullptr;
    VisitMap(map, 20, [&](unsigned char* element) {
        if (ue::At<Name>(element, 0) == key) { found = &ue::At<float>(element, 8); return false; }
        return true;
    });
    return found;
}
bool Layout(Object* component) {
    Object* collection = ue::FindObject("/Script/SGameAI.SGameAIParameterCollection");
    if (!collection || ue::StructSize(collection) != 880 ||
        std::memcmp(reinterpret_cast<const void*>(ue::Base() + kGetParameters), kGetParametersCode, sizeof(kGetParametersCode))) return false;
    normal_offset_ = ue::Offset(ue::ClassOf(component), "ParameterCollection");
    override_offset_ = ue::Offset(ue::ClassOf(component), "OverrideParameterCollection");
    override_flag_ = ue::Offset(ue::ClassOf(component), "ParameterCollectionOverriden");
    float_offset_ = ue::Offset(collection, "FloatParameters");
    return normal_offset_ == 1712 && override_offset_ == 824 && override_flag_ == 1704 && float_offset_ == 160;
}
void* Effective() {
    if (!Same(component_, component_name_)) return nullptr;
    return reinterpret_cast<void*(__fastcall*)(Object*)>(ue::Base() + kGetParameters)(component_);
}
float* Source(Name name, int32_t& collection_offset) {
    if (!Same(component_, component_name_)) return nullptr;
    const uint8_t flag = ue::At<uint8_t>(component_, override_flag_);
    if (flag > 1) return nullptr;
    if (flag) if (float* value = FindFloat(component_ + override_offset_ + float_offset_, name)) {
        collection_offset = override_offset_;
        return value;
    }
    collection_offset = normal_offset_;
    return FindFloat(component_ + normal_offset_ + float_offset_, name);
}
bool ReadEffective(Name name, float& value) {
    void* parameters = Effective();
    float* slot = parameters ? FindFloat(static_cast<unsigned char*>(parameters) + float_offset_, name) : nullptr;
    if (!slot || !std::isfinite(*slot)) return false;
    value = *slot;
    return true;
}
bool OwnMap(int32_t source) {
    // A live component should own its copied maps. Explicitly reject a backing
    // allocation shared with its strategy DataAsset rather than editing it.
    Call strategy(component_, "GetStrategyConfig");
    if (!strategy.Invoke()) return false;
    Object* asset = strategy.Get<Object*>("ReturnValue");
    if (!asset) return true;
    const int32_t collection = ue::Offset(ue::ClassOf(asset), "ParameterCollection");
    if (collection < 0) return false;
    const void* actual = ue::At<void*>(component_ + source + float_offset_, 0);
    const void* config = ue::At<void*>(asset + collection + float_offset_, 0);
    return actual && actual != config;
}
bool GetCooldown(const char* function, Name behavior, float& value) {
    if (!Same(cooldown_, cooldown_name_)) return false;
    Call get(cooldown_, function);
    if (!get.Set("BehaviorName", behavior).Invoke()) return false;
    value = get.Get<float>("ReturnValue");
    return get.Ok() && std::isfinite(value);
}
bool SetCooldown(const char* function, Name behavior, float value) {
    return Same(cooldown_, cooldown_name_) &&
        Call(cooldown_, function).Set("BehaviorName", behavior).Set("CooldownTime", value).Invoke();
}
bool Clock(float& value) {
    if (!Same(cooldown_, cooldown_name_)) return false;
    Call get(cooldown_, "GetTimeSeconds");
    if (!get.Invoke()) return false;
    value = get.Get<float>("ReturnValue");
    return get.Ok() && std::isfinite(value);
}
void Publish(const wchar_t* status) {
    Snapshot value;
    value.generation = generation_;
    value.ready = Same(pawn_, pawn_name_) && !rows_.empty();
    value.target = pawn_ ? WideName(pawn_name_) : std::wstring{};
    value.status = status;
    for (const Row& row : rows_) value.parameters.push_back(row.ui);
    AcquireSRWLockExclusive(&state_lock);
    state_ = std::move(value);
    ReleaseSRWLockExclusive(&state_lock);
}
void Refresh() {
    for (Row& row : rows_) {
        float value = 0;
        const bool read = row.cooldown_index >= 0 ? GetCooldown(row.ui.kind == Kind::BehaviorCooldown ?
            "GetBehaviorCooldownDefinition" : "GetBehaviorRemainingCooldown", row.name, value) : ReadEffective(row.name, value);
        if (read) row.ui.current = row.ui.kind == Kind::RemainingCooldown ? (std::max)(0.0f, value) : value;
    }
}
bool Restore() {
    bool ok = true;
    if (Same(component_, component_name_)) for (Row& row : rows_) if (row.cooldown_index < 0 && row.ui.changed) {
        float* slot = FindFloat(component_ + row.source_collection + float_offset_, row.name);
        // If the game rebuilt this source and removed the key, our modification
        // no longer exists. Do not add a key or alter another collection.
        if (slot) {
            *slot = row.ui.baseline;
            int32_t active_source = -1;
            float check = 0;
            if (!Near(*slot, row.ui.baseline) || !Source(row.name, active_source) ||
                (active_source == row.source_collection && (!ReadEffective(row.name, check) || !Near(check, row.ui.baseline)))) {
                ok = false; continue;
            }
        }
        row.ui.changed = false;
    }
    if (Same(cooldown_, cooldown_name_)) for (Cooldown& entry : cooldowns_) if (entry.changed) {
        float now = 0, definition = 0, remaining = 0;
        if (!Clock(now) || !SetCooldown("SetBehaviorCooldownDefinition", entry.name, entry.original_definition)) { ok = false; continue; }
        const float expected = (std::max)(0.0f, entry.original_remaining - (std::max)(0.0f, now - entry.clock));
        if (!SetCooldown("SetBehaviorRemainingCooldown", entry.name, expected) ||
            !GetCooldown("GetBehaviorCooldownDefinition", entry.name, definition) || !Near(definition, entry.original_definition) ||
            !GetCooldown("GetBehaviorRemainingCooldown", entry.name, remaining) || !Near((std::max)(0.0f, remaining), expected, 0.1f)) {
            ok = false; continue;
        }
        entry.changed = false;
    }
    for (Row& row : rows_) if (row.cooldown_index >= 0 && !cooldowns_[static_cast<size_t>(row.cooldown_index)].changed) row.ui.changed = false;
    Refresh();
    Publish(ok ? L"已恢复这个实例修改前的 AI 参数" : L"部分 AI 参数恢复未通过读回验证；保留目标以便重试");
    return ok;
}
bool Set(int32_t index, float value) {
    if (index < 0 || static_cast<size_t>(index) >= rows_.size() || !Same(pawn_, pawn_name_)) return false;
    Row& row = rows_[static_cast<size_t>(index)];
    if (!std::isfinite(value) || value < row.ui.minimum || value > row.ui.maximum) return false;
    if (row.cooldown_index < 0) {
        int32_t source = -1;
        float* slot = Source(row.name, source);
        if (!slot || source != row.source_collection || !OwnMap(source)) return false;
        if (!row.ui.changed) row.ui.baseline = *slot;
        *slot = value;
        row.ui.changed = true;
        float check = 0;
        if (!ReadEffective(row.name, check) || !Near(check, value)) return false;
    } else {
        Cooldown& entry = cooldowns_[static_cast<size_t>(row.cooldown_index)];
        if (!entry.changed) {
            if (!GetCooldown("GetBehaviorCooldownDefinition", row.name, entry.original_definition) ||
                !GetCooldown("GetBehaviorRemainingCooldown", row.name, entry.original_remaining) || !Clock(entry.clock)) return false;
            for (Row& related : rows_) if (related.cooldown_index == row.cooldown_index)
                related.ui.baseline = related.ui.kind == Kind::BehaviorCooldown ? entry.original_definition : (std::max)(0.0f, entry.original_remaining);
        }
        entry.changed = true; // Track even a failed readback so Restore can undo it.
        row.ui.changed = true;
        const bool definition = row.ui.kind == Kind::BehaviorCooldown;
        if (!SetCooldown(definition ? "SetBehaviorCooldownDefinition" : "SetBehaviorRemainingCooldown", row.name, value)) return false;
        float check = 0;
        if (!GetCooldown(definition ? "GetBehaviorCooldownDefinition" : "GetBehaviorRemainingCooldown", row.name, check) ||
            !Near(definition ? check : (std::max)(0.0f, check), value, definition ? 0.001f : 0.1f)) return false;
    }
    Refresh();
    Publish(L"原生实例参数已修改并读回；实际攻击频率需在战斗中观察");
    if (logger_) {
        char line[200]{};
        sprintf_s(line, "BOSS_AI_VALUE generation=%llu index=%d kind=%d requested=%.6g readback=%.6g",
            static_cast<unsigned long long>(generation_), index, static_cast<int>(row.ui.kind), value, row.ui.current);
        logger_(line);
    }
    return true;
}
void DiscoverCooldown(Object* controller) {
    if (!controller || Same(cooldown_, cooldown_name_)) return;
    if (cooldown_) {
        // Its original instance was destroyed. Drop only those obsolete rows;
        // keep live component edits and invalidate UI requests for old indices.
        rows_.erase(std::remove_if(rows_.begin(), rows_.end(), [](const Row& row) { return row.cooldown_index >= 0; }), rows_.end());
        for (size_t i = 0; i < rows_.size(); ++i) rows_[i].ui.index = static_cast<int32_t>(i);
        cooldowns_.clear(); cooldown_ = nullptr; ++generation_;
    }
    Object* utility = ue::Default("/Script/SGameAI.Default__SGameAIFunctionLibrary");
    Call get(utility, "GetCooldownObject");
    if (get.Set("AIController", controller).Invoke()) cooldown_ = get.Get<Object*>("ReturnValue");
    Object* cooldown_class = ue::FindObject("/Script/SGameAI.SGameAICooldownObject");
    if (cooldown_ && cooldown_class && ue::Alive(cooldown_) && ue::IsA(cooldown_, cooldown_class) && !ue::IsTemplate(cooldown_) &&
        Property(cooldown_, "AIController") == controller) {
        cooldown_name_ = ue::NameOf(cooldown_);
        const int32_t definitions = ue::Offset(ue::ClassOf(cooldown_), "BehaviorDefinition");
        if (definitions >= 0) VisitMap(cooldown_ + definitions, 64, [&](unsigned char* element) {
            const Name name = ue::At<Name>(element, 0);
            const std::wstring key = WideName(name);
            float definition = 0, remaining = 0;
            if (name.None() || key.empty() || !GetCooldown("GetBehaviorCooldownDefinition", name, definition) ||
                !GetCooldown("GetBehaviorRemainingCooldown", name, remaining)) return true;
            const int32_t cooldown_index = static_cast<int32_t>(cooldowns_.size());
            cooldowns_.push_back({name, definition, remaining, 0, false});
            for (const bool is_definition : {true, false}) {
                Row row;
                row.name = name; row.cooldown_index = cooldown_index;
                const float current = is_definition ? definition : (std::max)(0.0f, remaining);
                row.ui = {static_cast<int32_t>(rows_.size()), is_definition ? Kind::BehaviorCooldown : Kind::RemainingCooldown,
                    key, (is_definition ? L"行为冷却 / " : L"当前剩余冷却 / ") + key, L"秒",
                    L"此 AIController 的 SGameAICooldownObject 原生 Get/SetBehaviorCooldown 接口", current, current, 0, 600, false};
                rows_.push_back(std::move(row));
            }
            return true;
        });
    } else cooldown_ = nullptr;
}
void Tick() {
    Request pending{};
    AcquireSRWLockExclusive(&queue_lock);
    const bool have = !queue_.empty();
    if (have) { pending = queue_.front(); queue_.pop_front(); }
    ReleaseSRWLockExclusive(&queue_lock);
    if (!have) return;
    if (pending.generation != generation_) { Publish(L"Boss 实例已改变，旧参数请求已拒绝"); return; }
    if (pending.action == Action::Restore) { Restore(); return; }
    if (pending.action == Action::Refresh) {
        if (rows_.empty() && Same(pawn_, pawn_name_)) { BindTarget(pawn_); return; }
        DiscoverCooldown(Property(pawn_, "Controller"));
        Refresh(); Publish(L"已读取当前实例 AI 参数"); return;
    }
    if (!Set(pending.index, pending.value)) { Publish(L"参数未通过原生读回；仍可恢复已跟踪修改"); Log("BOSS_AI set refused or native readback mismatch"); }
}
bool Queue(uint64_t generation, Action action, int32_t index = -1, float value = 0) {
    AcquireSRWLockExclusive(&queue_lock);
    const bool ok = queue_.size() < 32;
    if (ok) queue_.push_back({generation, action, index, value});
    ReleaseSRWLockExclusive(&queue_lock);
    return ok;
}
}

void Initialize(Logger logger) { logger_ = logger; Publish(L"等待原生 Boss AI 实例"); }
bool ReleaseTarget() {
    if (!Restore()) return false;
    pawn_ = component_ = cooldown_ = nullptr;
    rows_.clear(); cooldowns_.clear(); ++generation_;
    Publish(L"等待原生 Boss AI 实例");
    return true;
}
bool BindTarget(Object* boss) {
    if (boss && Same(pawn_, pawn_name_) && pawn_ == boss && !rows_.empty()) return true;
    if (!ReleaseTarget() || !boss || !ue::Alive(boss) || ue::IsTemplate(boss)) return false;
    Object* controller = Property(boss, "Controller");
    Object* component = Property(controller, "SGameAIComponent");
    Object* component_class = ue::FindObject("/Script/SGameAI.SGameAIComponent");
    if (!component || !component_class || !ue::IsA(component, component_class) || ue::IsTemplate(component) ||
        Property(component, "Pawn") != boss || !Layout(component)) { Publish(L"该角色没有已确认的 SGameAI 实例参数接口"); return false; }
    pawn_ = boss; pawn_name_ = ue::NameOf(boss);
    component_ = component; component_name_ = ue::NameOf(component);
    for (const KnownFloat& known : kKnown) {
        const Name name = ue::MakeName(known.key);
        int32_t source = -1;
        float* slot = Source(name, source);
        float effective = 0;
        if (name.None() || !slot || !OwnMap(source) || !ReadEffective(name, effective) || !Near(*slot, effective)) continue;
        Row row;
        row.name = name; row.source_collection = source;
        row.ui = {static_cast<int32_t>(rows_.size()), known.kind, known.key, known.key,
            known.kind == Kind::Probability ? L"概率 0–1" : L"相对权重", source == override_offset_ ?
            L"AIComponent 实例 OverrideParameterCollection.FloatParameters" : L"AIComponent 实例 ParameterCollection.FloatParameters",
            effective, effective, 0, known.maximum, false};
        rows_.push_back(std::move(row));
    }
    DiscoverCooldown(controller);
    Publish(rows_.empty() ? L"当前 AI 未提供已确认的概率/权重或行为冷却项" :
        L"仅修改此 Boss 实例；权重上限10000、冷却上限600秒为界面约束，非原生硬上限");
    Log("BOSS_AI bound native instance; discovered actual probability/weight keys and per-controller behavior cooldowns");
    return !rows_.empty();
}
void OnGameThreadGuarded() {
    __try { Tick(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Publish(L"AI 参数处理异常；已记录的修改仍保留用于恢复");
        Log("BOSS_AI request fault; no template changes performed");
    }
}
Snapshot GetSnapshot() {
    AcquireSRWLockShared(&state_lock); Snapshot value = state_; ReleaseSRWLockShared(&state_lock); return value;
}
bool RequestSet(uint64_t generation, int32_t index, float value) { return std::isfinite(value) && Queue(generation, Action::Set, index, value); }
bool RequestRefresh(uint64_t generation) { return Queue(generation, Action::Refresh); }
bool RequestRestore(uint64_t generation) { return Queue(generation, Action::Restore); }
}
