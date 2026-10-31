#include "character_swap.h"
#include "ue_runtime.h"
#include <windows.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <sstream>

namespace CharacterSwap {
namespace {
using ue::Object; using ue::Call; using ue::Name;
struct Preset { const char* id; const wchar_t* name; const wchar_t* detail; bool advanced; };
#include "character_presets.generated.h"
SRWLOCK state_lock = SRWLOCK_INIT, request_lock = SRWLOCK_INIT;
Snapshot snapshot_;
Logger logger_ = nullptr;
ReleaseExternalAppearance release_external_ = nullptr;
Object* engine_ = nullptr;
Object* component_ = nullptr;
Name component_name_, blocked_world_name_;
Object* blocked_world_ = nullptr;
bool initialized_ = false, enabled_ = false, owns_suppression_ = false, changed_logic_ = false, original_configured_ = false;
bool restore_ended_ = false, restore_pending_ = false, waiting_external_ = false;
Name original_current_, original_effective_;
volatile LONG disabled_ = 0;
int32_t selected_ = -1, pending_action_ = -1, pending_entry_ = -1;
int32_t waiting_entry_ = -1;
DWORD faults_ = 0, fault_code_ = 0;
uintptr_t fault_address_ = 0;
ULONGLONG next_tick_ = 0;
ULONGLONG waiting_deadline_ = 0;

void Logf(const char* format, ...) {
    if (!logger_) return;
    char line[800]{}; va_list arguments; va_start(arguments, format); vsprintf_s(line, format, arguments); va_end(arguments); logger_(line);
}
void Status(const wchar_t* format, ...) {
    wchar_t text[640]{}; va_list arguments; va_start(arguments, format); vswprintf_s(text, format, arguments); va_end(arguments);
    AcquireSRWLockExclusive(&state_lock); snapshot_.status = text; ReleaseSRWLockExclusive(&state_lock);
}
void Active(bool active, bool busy = false) {
    AcquireSRWLockExclusive(&state_lock); snapshot_.active = active; snapshot_.busy = busy || restore_pending_; ReleaseSRWLockExclusive(&state_lock);
}
bool Same(Object* object, Name name) { return ue::Alive(object) && ue::NameOf(object) == name; }
Object* Class(const char* path) { return ue::FindObject(path); }
int32_t Offset(const char* property) { return ue::Offset(Class("/Script/ProjectPBZ.PBZAppearanceComponent"), property); }
template<class T> T ReadProperty(Object* object, const char* class_path, const char* property) {
    const int32_t offset = ue::Offset(Class(class_path), property);
    return object && offset >= 0 ? ue::At<T>(object, offset) : T{};
}
struct Context { Object* world = nullptr; Object* pawn = nullptr; Object* appearance = nullptr; };
bool CurrentContext(Context& context) {
    if (!ue::Alive(engine_)) engine_ = ue::FindEngine();
    if (!engine_) return false;
    Object* viewport = ReadProperty<Object*>(engine_, "/Script/Engine.Engine", "GameViewport");
    context.world = ReadProperty<Object*>(viewport, "/Script/Engine.GameViewportClient", "World");
    if (!ue::Alive(context.world)) return false;
    Object* instance = ReadProperty<Object*>(context.world, "/Script/Engine.World", "OwningGameInstance");
    const ue::Array players = ReadProperty<ue::Array>(instance, "/Script/Engine.GameInstance", "LocalPlayers");
    if (players.num < 1 || players.num > 16 || !players.data) return false;
    Object* local = static_cast<Object**>(players.data)[0];
    Object* controller = ReadProperty<Object*>(local, "/Script/Engine.Player", "PlayerController");
    context.pawn = ReadProperty<Object*>(controller, "/Script/Engine.Controller", "Pawn");
    if (!ue::Alive(context.pawn)) return false;
    Call components(context.pawn, "K2_GetComponentsByClass");
    Object* appearance_class = Class("/Script/ProjectPBZ.PBZAppearanceComponent");
    if (!appearance_class || !components.Set("ComponentClass", appearance_class).Invoke()) return false;
    const ue::Array found = components.Get<ue::Array>("ReturnValue");
    if (found.num > 0 && found.num < 64 && found.data)
        for (int32_t i = 0; i < found.num; ++i) if (ue::Alive(static_cast<Object**>(found.data)[i])) { context.appearance = static_cast<Object**>(found.data)[i]; break; }
    ue::Free(found.data);
    return context.appearance != nullptr;
}
bool Validate(Object* appearance) {
    struct Field { const char* name; int32_t offset; };
    const Field fields[] = {{"DoPBZAppearanceLogic", 0xc8}, {"bSuppressAppearanceSave", 0xc9},
        {"bUseConfiguredAppearance", 0x188}, {"ConfiguredOutfit", 0x18c}, {"CurrentOutfit", 0x278}, {"CurrentSkinColor", 0x288}};
    for (const auto& field : fields) if (Offset(field.name) != field.offset) return false;
    Call begin(appearance, "BeginSuppressAppearanceSave"), end(appearance, "EndSuppressAppearanceSave");
    Call equip(appearance, "EquipOutfit"), effective(appearance, "GetEffectiveOutfit");
    return begin.Ok() && end.Ok() && end.Slot("bApplyChanges", sizeof(bool)) && equip.Slot("OutfitID", sizeof(Name)) &&
        equip.Slot("bIsPreview", sizeof(bool)) && effective.Slot("ReturnValue", sizeof(Name));
}
Name Effective(Object* appearance) {
    Call call(appearance, "GetEffectiveOutfit");
    return call.Invoke() ? call.Get<Name>("ReturnValue") : Name{};
}
std::wstring OutfitName(Name name) {
    for (const auto& preset : kPresets) if (ue::NameIs(name, preset.id)) return preset.name;
    char text[160]{}; ue::NameText(name, text, sizeof(text)); const std::string value(text);
    return std::wstring(value.begin(), value.end());
}
void ClearSession() {
    component_ = nullptr; component_name_ = {}; owns_suppression_ = false; changed_logic_ = false;
    original_configured_ = false; original_current_ = {}; original_effective_ = {}; restore_ended_ = false; restore_pending_ = false;
}
bool RestoreSession() {
    if (!owns_suppression_) return true;
    if (!Same(component_, component_name_)) {
        Logf("CHARACTER_SWAP temporary appearance component was unloaded; no stale-object call was made");
        ClearSession(); return true;
    }
    Object* appearance = component_;
    if (!Validate(appearance)) { restore_pending_ = true; return false; }
    const bool suppressed = ue::At<uint8_t>(appearance, Offset("bSuppressAppearanceSave")) != 0;
    if (!suppressed && !restore_ended_) {
        Logf("CHARACTER_SWAP suppression ended externally; preserved external appearance and retired ownership");
        ClearSession(); return true;
    }
    // Configured appearance is not part of the engine's suppression backup.
    if (original_configured_ && !ue::At<uint8_t>(appearance, Offset("bUseConfiguredAppearance")))
        ue::At<uint8_t>(appearance, Offset("bUseConfiguredAppearance")) = 1;
    if (suppressed) {
        Call end(appearance, "EndSuppressAppearanceSave");
        if (!end.Set("bApplyChanges", false).Invoke() || ue::At<uint8_t>(appearance, Offset("bSuppressAppearanceSave"))) {
            restore_pending_ = true;
            Logf("CHARACTER_SWAP EndSuppressAppearanceSave(false) incomplete; retained owned session"); return false;
        }
        restore_ended_ = true;
    }
    if (!(ue::At<Name>(appearance, Offset("CurrentOutfit")) == original_current_) || !(Effective(appearance) == original_effective_)) {
        restore_pending_ = true; Logf("CHARACTER_SWAP restore outfit readback differs; retained baseline"); return false;
    }
    if (changed_logic_ && ue::At<uint8_t>(appearance, Offset("DoPBZAppearanceLogic")))
        ue::At<uint8_t>(appearance, Offset("DoPBZAppearanceLogic")) = 0;
    ClearSession();
    Logf("CHARACTER_SWAP restore verified (EndSuppressAppearanceSave false; original outfit readback; no apply/save)");
    return true;
}
bool Apply(const Context& context, int32_t entry) {
    if (entry < 0 || static_cast<size_t>(entry) >= std::size(kPresets)) return false;
    waiting_external_ = false;
    if (restore_pending_ && !RestoreSession()) { waiting_external_ = true; Status(L"上次主角外观恢复尚未完成，保留原会话等待重试"); return false; }
    if (release_external_ && !release_external_()) {
        waiting_external_ = true; Status(L"等待 NPC 原模型空闲并完整恢复，再装配主角套装"); return false;
    }
    Object* appearance = context.appearance;
    if (!Validate(appearance)) { Status(L"原生外观接口或布局不符，未更换模型"); return false; }
    if (owns_suppression_ && !Same(component_, component_name_)) {
        RestoreSession(); enabled_ = false;
        Status(L"上个临时角色已卸载；已停止覆盖，请在当前场景重新应用模型"); return false;
    }
    if (owns_suppression_ && component_ != appearance && !RestoreSession()) {
        waiting_external_ = true; Status(L"旧角色的临时外观会话未能恢复，已停止更换"); return false;
    }
    if (!owns_suppression_) {
        if (ue::At<uint8_t>(appearance, Offset("bSuppressAppearanceSave"))) {
            Status(L"游戏正处于其它外观预览会话，请先关闭该界面再替换"); return false;
        }
        const Name original = Effective(appearance);
        if (original.None()) { Status(L"主角外观尚未初始化，请稍后再试"); return false; }
        original_configured_ = ue::At<uint8_t>(appearance, Offset("bUseConfiguredAppearance")) != 0;
        original_current_ = ue::At<Name>(appearance, Offset("CurrentOutfit")); original_effective_ = original;
        restore_ended_ = false;
        changed_logic_ = ue::At<uint8_t>(appearance, Offset("DoPBZAppearanceLogic")) == 0;
        Call begin(appearance, "BeginSuppressAppearanceSave");
        if (!begin.Invoke() || !ue::At<uint8_t>(appearance, Offset("bSuppressAppearanceSave"))) {
            Status(L"无法开始临时外观会话，未更换模型"); return false;
        }
        component_ = appearance; component_name_ = ue::NameOf(appearance); owns_suppression_ = true;
        const std::wstring original_name = OutfitName(original);
        AcquireSRWLockExclusive(&state_lock); snapshot_.original = original_name; ReleaseSRWLockExclusive(&state_lock);
    } else if (!ue::At<uint8_t>(appearance, Offset("bSuppressAppearanceSave"))) {
        Status(L"游戏已结束临时外观会话；请先恢复，再重新应用"); return false;
    }
    ue::At<uint8_t>(appearance, Offset("DoPBZAppearanceLogic")) = 1;
    ue::At<uint8_t>(appearance, Offset("bUseConfiguredAppearance")) = 0;
    const auto& preset = kPresets[entry];
    const std::wstring id(preset.id, preset.id + std::strlen(preset.id));
    const Name outfit = ue::MakeName(id.c_str());
    Call equip(appearance, "EquipOutfit");
    if (outfit.None() || !equip.Set("OutfitID", outfit).Set("bIsPreview", false).Invoke() || !(Effective(appearance) == outfit)) {
        RestoreSession(); enabled_ = false; Active(false);
        Status(L"原生外观装配未接受请求，已尝试恢复原模型"); return false;
    }
    selected_ = entry; enabled_ = true;
    AcquireSRWLockExclusive(&state_lock); snapshot_.selected = preset.name; ReleaseSRWLockExclusive(&state_lock);
    Active(true);
    Status(L"已由原生外观系统装配：%s。保留主角动画；可按恢复按钮还原", preset.name);
    Logf("CHARACTER_SWAP applied id=%s component=%p suppression=1 temporary=1", preset.id, static_cast<void*>(appearance));
    return true;
}
void Tick() {
    if (!initialized_) return;
    int32_t action = -1, entry = -1;
    AcquireSRWLockExclusive(&request_lock);
    action = pending_action_; entry = pending_entry_; pending_action_ = -1;
    ReleaseSRWLockExclusive(&request_lock);
    if (action == static_cast<int32_t>(Action::Replace)) {
        waiting_entry_ = entry; waiting_deadline_ = GetTickCount64() + 60000;
    }
    if (action == static_cast<int32_t>(Action::Restore)) {
        waiting_entry_ = -1; enabled_ = false; selected_ = -1; blocked_world_ = nullptr;
        const bool restored = RestoreSession(); Active(false);
        AcquireSRWLockExclusive(&state_lock); snapshot_.selected.clear(); ReleaseSRWLockExclusive(&state_lock);
        Status(restored ? L"临时主角外观会话已释放，没有提交外观保存" : L"恢复尚未完整；原组件和外观基线已保留，等待重试");
    }
    if (action < 0 && GetTickCount64() < next_tick_) return;
    next_tick_ = GetTickCount64() + 500;
    if (InterlockedCompareExchange(&disabled_, 0, 0)) { Active(false); return; }
    if (restore_pending_ && !RestoreSession()) { Active(false, true); return; }
    Context context;
    const bool context_ok = CurrentContext(context);
    const bool ready = context_ok && Validate(context.appearance);
    AcquireSRWLockExclusive(&state_lock); snapshot_.ready = ready; ReleaseSRWLockExclusive(&state_lock);
    if (!ready) {
        if (waiting_entry_ >= 0) {
            if (GetTickCount64() > waiting_deadline_) { waiting_entry_ = -1; Active(enabled_); Status(L"等待可用主角外观组件超时，未更换模型"); }
            else { Active(enabled_, true); Status(L"等待当前场景的可用主角外观组件"); }
        }
        return;
    }
    if (blocked_world_) {
        if (context.world == blocked_world_ && ue::NameOf(context.world) == blocked_world_name_) return;
        blocked_world_ = nullptr;
        if (enabled_ && selected_ >= 0 && waiting_entry_ < 0) { waiting_entry_ = selected_; waiting_deadline_ = GetTickCount64() + 60000; }
    }
    if (owns_suppression_ && (!Same(component_, component_name_) || context.appearance != component_)) {
        if (Same(component_, component_name_)) RestoreSession();
        else ClearSession();
        enabled_ = false; Active(false);
        Status(L"角色或地图已变化，已停止临时覆盖；请在当前场景重新应用模型");
    }
    if (waiting_entry_ >= 0) {
        if (GetTickCount64() > waiting_deadline_) { waiting_entry_ = -1; Active(enabled_); Status(L"等待 NPC 恢复 / 主角外观释放超时，未继续更换"); return; }
        Active(enabled_, true);
        const bool applied = Apply(context, waiting_entry_);
        if (applied || !waiting_external_) waiting_entry_ = -1;
        Active(enabled_, waiting_entry_ >= 0);
    }
}
int FaultFilter(const EXCEPTION_POINTERS* exception) {
    fault_code_ = exception->ExceptionRecord->ExceptionCode;
    fault_address_ = reinterpret_cast<uintptr_t>(exception->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;
}
void RecoverGuarded() {
    __try { RestoreSession(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { restore_pending_ = owns_suppression_; }
}
void GuardedTick() {
    __try { Tick(); }
    __except (FaultFilter(GetExceptionInformation())) {
        ++faults_; enabled_ = false; waiting_entry_ = -1; RecoverGuarded(); Active(false);
        if (faults_ >= 3) {
            InterlockedExchange(&disabled_, 1);
            AcquireSRWLockExclusive(&state_lock); snapshot_.ready = false; ReleaseSRWLockExclusive(&state_lock);
        }
        Status(L"角色替换发生错误 0x%08lx，已停止当前操作；Boss Rush 与头发调试继续运行", fault_code_);
        Logf("CHARACTER_SWAP fault %lu/3 code=0x%08lx address=%p disabled=%d", faults_, fault_code_, reinterpret_cast<void*>(fault_address_), InterlockedCompareExchange(&disabled_, 0, 0) ? 1 : 0);
    }
}
void Travel() {
    if (!enabled_ || !owns_suppression_) return;
    Context context;
    if (!CurrentContext(context)) return;
    blocked_world_ = context.world; blocked_world_name_ = ue::NameOf(context.world);
    if (!RestoreSession()) { enabled_ = false; Active(false); Status(L"切图前恢复临时外观失败，已停止角色覆盖"); return; }
    Status(L"切图前已恢复临时外观状态；等待新场景后重新装配所选模型");
}
}

void Initialize(Logger logger, ReleaseExternalAppearance release_external) {
    logger_ = logger; release_external_ = release_external;
    Snapshot state;
    for (size_t i = 0; i < std::size(kPresets); ++i)
        state.entries.push_back({static_cast<int32_t>(i), kPresets[i].id, kPresets[i].name, kPresets[i].detail, kPresets[i].advanced});
    AcquireSRWLockExclusive(&state_lock); snapshot_ = std::move(state); ReleaseSRWLockExclusive(&state_lock);
    initialized_ = true;
    Logf("CHARACTER_SWAP %zu native player outfit presets initialized; temporary appearance only", std::size(kPresets));
}
bool Request(Action action, int32_t entry) {
    if (action == Action::Replace && (entry < 0 || static_cast<size_t>(entry) >= std::size(kPresets))) return false;
    if (InterlockedCompareExchange(&disabled_, 0, 0) && action != Action::Restore) return false;
    AcquireSRWLockExclusive(&request_lock); pending_action_ = static_cast<int32_t>(action); pending_entry_ = entry; ReleaseSRWLockExclusive(&request_lock);
    Status(L"角色替换请求已排队，等待游戏线程处理"); return true;
}
Snapshot GetSnapshot() {
    AcquireSRWLockShared(&state_lock); Snapshot copy = snapshot_; ReleaseSRWLockShared(&state_lock); return copy;
}
void OnGameThreadGuarded() { GuardedTick(); }
bool ReleaseForExternal() {
    AcquireSRWLockExclusive(&request_lock); pending_action_ = -1; ReleaseSRWLockExclusive(&request_lock);
    __try {
        waiting_entry_ = -1; enabled_ = false; selected_ = -1; blocked_world_ = nullptr;
        if (!RestoreSession()) { Active(false, true); Status(L"主角临时外观仍待恢复，阻止 NPC 接管"); return false; }
        Context context;
        if (!CurrentContext(context) || !Validate(context.appearance)) { Active(false); return false; }
        if (ue::At<uint8_t>(context.appearance, Offset("bSuppressAppearanceSave"))) {
            Active(false, true); Status(L"游戏的其它外观预览仍持有会话，NPC 暂不能接管"); return false;
        }
        Active(false);
        AcquireSRWLockExclusive(&state_lock); snapshot_.selected.clear(); ReleaseSRWLockExclusive(&state_lock);
        return true;
    } __except (FaultFilter(GetExceptionInformation())) {
        restore_pending_ = owns_suppression_; Active(false, true);
        Logf("CHARACTER_SWAP external release fault 0x%08lx; retained live ownership", fault_code_); return false;
    }
}
void BeforeTravel() {
    __try { Travel(); }
    __except (FaultFilter(GetExceptionInformation())) { enabled_ = false; RecoverGuarded(); Active(false); Logf("CHARACTER_SWAP pre-travel restore fault 0x%08lx", fault_code_); }
}
bool HandleCommand(const char* verb, const char* arguments) {
    if (std::strcmp(verb, "model")) return false;
    std::istringstream input(arguments ? arguments : ""); std::string action, id; input >> action >> id;
    if (action == "restore") return Request(Action::Restore);
    if (action == "list") { for (size_t i = 0; i < std::size(kPresets); ++i) Logf("CHARACTER_MODEL %zu id=%s name=%ls", i, kPresets[i].id, kPresets[i].name); return true; }
    if (action == "status") { const Snapshot snapshot = GetSnapshot(); Logf("CHARACTER_STATUS %ls", snapshot.status.c_str()); return true; }
    if (action == "apply") for (size_t i = 0; i < std::size(kPresets); ++i)
        if (id == kPresets[i].id || id == std::to_string(i)) return Request(Action::Replace, static_cast<int32_t>(i));
    return false;
}
}
