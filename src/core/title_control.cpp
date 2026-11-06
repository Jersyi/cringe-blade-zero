#include "title_control.h"
#include "hub_pause.h"
#include "ue_runtime.h"
#include <windows.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

namespace TitleControl {
namespace {
using ue::Object;
using ue::Call;
Logger logger_ = nullptr;
enum Request : int { None, Continue, Quit, Probe };
std::atomic<int> request_{None};
std::atomic<bool> keepalive_{false};
// Game thread only.
Object* engine_ = nullptr;
ULONGLONG next_check_ = 0;
unsigned lifted_ = 0;

void Logf(const char* format, ...) {
    if (!logger_) return;
    char text[700]{}; va_list args; va_start(args, format); _vsnprintf_s(text, sizeof(text), _TRUNCATE, format, args); va_end(args);
    logger_(text);
}
Object* Property(Object* object, const char* name) {
    if (!ue::Alive(object)) return nullptr;
    const int32_t offset = ue::Offset(ue::ClassOf(object), name);
    return offset >= 0 ? ue::At<Object*>(object, offset) : nullptr;
}
Object* CurrentWorld() {
    if (!ue::Alive(engine_)) engine_ = ue::FindEngine();
    Object* world = Property(Property(engine_, "GameViewport"), "World");
    return ue::Alive(world) ? world : nullptr;
}
Object* Gameplay() { return ue::Default("/Script/Engine.Default__GameplayStatics"); }
std::string NameOfObject(Object* object) {
    if (!object) return "-";
    char text[160]{}; ue::NameText(ue::NameOf(object), text, sizeof(text)); return text;
}
// "name:size name:size" of a UFunction's parameters (its child properties).
std::string Parameters(Object* function) {
    std::string out;
    if (!function) return "missing";
    for (unsigned char* p = ue::At<unsigned char*>(function, ue::layout::kChildProperties); p; p = ue::At<unsigned char*>(p, ue::layout::kPropNext)) {
        char name[96]{}; ue::NameText(ue::At<ue::Name>(p, ue::layout::kPropName), name, sizeof(name));
        char item[128]{}; _snprintf_s(item, sizeof(item), _TRUNCATE, "%s%s:%d", out.empty() ? "" : " ", name, ue::At<int32_t>(p, ue::layout::kPropElementSize));
        out += item;
        if (out.size() > 300) break;
    }
    return out.empty() ? "none" : out;
}
Object* GameMode(Object* world) {
    Call get(Gameplay(), "GetGameMode");
    return get.Set("WorldContextObject", world).Invoke() ? get.Get<Object*>("ReturnValue") : nullptr;
}
bool IsPaused(Object* world, bool& value) {
    Call get(Gameplay(), "IsGamePaused");
    if (!get.Slot("ReturnValue", 1) || !get.Set("WorldContextObject", world).Invoke()) return false;
    value = get.Get<bool>("ReturnValue"); return true;
}
bool SetPaused(Object* world, bool value) {
    Call set(Gameplay(), "SetGamePaused");
    return set.Slot("ReturnValue", 1) && set.Set("WorldContextObject", world).Set("bPaused", value).Invoke() && set.Get<bool>("ReturnValue");
}

std::string Narrow(const ue::String& text) {
    std::string out;
    for (int32_t i = 0; text.data && i < text.num && text.data[i]; ++i) out += text.data[i] < 128 ? char(text.data[i]) : '?';
    return out;
}
// The live PBZSaveLoadSubsystem of the game instance (SubsystemBlueprintLibrary::GetGameInstanceSubsystem).
Object* SaveSubsystem(Object* world) {
    Object* cls = ue::FindObject("/Script/ProjectPBZ.PBZSaveLoadSubsystem");
    if (!cls) return nullptr;
    Call get(ue::Default("/Script/Engine.Default__SubsystemBlueprintLibrary"), "GetGameInstanceSubsystem");
    if (!get.Set("ContextObject", world).Set("Class", cls).Invoke()) return nullptr;
    Object* subsystem = get.Get<Object*>("ReturnValue");
    return ue::Alive(subsystem) ? subsystem : nullptr;
}
ULONGLONG continued_at_ = 0;  // game thread only

// 继续游戏 on the start menu (WB_StartMenu: ContinueGame clicked -> PBZSaveLoadSubsystem::LoadGameOnStart). The slot
// is the one the menu continues (GetContinueSlotName). Repeats within 90 s of a started load are ignored: the runner
// asks again every 15 s until the hero is ready, and a second load while the first is travelling must not happen.
void DoContinue(Object* world) {
    Object* mode = GameMode(world);
    const std::string mode_name = NameOfObject(mode ? ue::ClassOf(mode) : nullptr);
    if (mode_name.find("StartMenu") == std::string::npos) { Logf("TITLE continue skipped: mode=%s is not the start menu", mode_name.c_str()); return; }
    const ULONGLONG now = GetTickCount64();
    if (continued_at_ && now - continued_at_ < 90000) { Logf("TITLE continue skipped: a load started %llu ms ago", now - continued_at_); return; }
    Object* subsystem = SaveSubsystem(world);
    if (!subsystem) { Logf("TITLE continue failed: no PBZSaveLoadSubsystem"); return; }
    Call slot(subsystem, "GetContinueSlotName");
    if (!slot.Invoke()) { Logf("TITLE continue failed: GetContinueSlotName"); return; }
    const ue::String name = slot.Get<ue::String>("ReturnValue");
    const std::string text = Narrow(name);
    if (text.empty()) { Logf("TITLE continue failed: no continue slot (no save?)"); if (name.data) ue::Free(const_cast<wchar_t*>(name.data)); return; }
    // The native thunk copies SlotName out of the parameter block, so the string stays ours and is freed below.
    Call load(subsystem, "LoadGameOnStart");
    const bool ok = load.Set("SlotName", name).Invoke();
    if (ok) continued_at_ = now;
    Logf("TITLE continue slot=%s via=PBZSaveLoadSubsystem.LoadGameOnStart ok=%d", text.c_str(), ok ? 1 : 0);
    ue::Free(const_cast<wchar_t*>(name.data));
}
void DoQuit(Object* world) {
    Call quit(ue::Default("/Script/Engine.Default__KismetSystemLibrary"), "QuitGame");
    const bool ok = quit.Set("WorldContextObject", world).Invoke();
    Logf("TITLE quit ok=%d", ok ? 1 : 0);
}
void DoProbe(Object* world) {
    Object* mode = GameMode(world);
    Object* cls = mode ? ue::ClassOf(mode) : nullptr;
    bool paused = false; const bool known = IsPaused(world, paused);
    Logf("TITLE probe world=%s mode=%s paused=%s keepalive=%d", NameOfObject(world).c_str(), NameOfObject(cls).c_str(), known ? (paused ? "1" : "0") : "?",
         keepalive_.load() ? 1 : 0);
    for (const char* name : {"ContinueGame", "BP_ContinueGame", "BP_StartMenuGame"})
        Logf("TITLE probe mode.%s params=%s", name, cls ? Parameters(ue::FindFunction(cls, name)).c_str() : "-");
    for (const char* path : {"/Script/ProjectPBZ.PBZSaveLoadSubsystem", "/Script/ProjectPBZ.PBZGameInstance"}) {
        Object* c = ue::FindObject(path);
        for (const char* name : {"GetContinueSlotName", "LoadGameFromSlot", "LoadGame", "LoadGameOnStart"})
            if (Object* f = c ? ue::FindFunction(c, name) : nullptr) Logf("TITLE probe %s.%s params=%s", path, name, Parameters(f).c_str());
    }
}
}

void Initialize(Logger logger) { logger_ = logger; }
bool HandleCommand(const char* verb, const char* argument) {
    if (std::strcmp(verb, "title")) return false;
    if (!std::strcmp(argument, "continue")) request_.store(Continue);
    else if (!std::strcmp(argument, "quit")) request_.store(Quit);
    else if (!std::strcmp(argument, "probe")) request_.store(Probe);
    else if (!std::strcmp(argument, "keepalive on") || !std::strcmp(argument, "keepalive off")) {
        keepalive_.store(!std::strcmp(argument, "keepalive on")); Logf("TITLE keepalive %s", keepalive_.load() ? "on" : "off");
    } else return false;
    return true;
}
void OnGameThread() {
    Object* world = CurrentWorld();
    if (!world) return;
    switch (request_.exchange(None)) {
    case Continue: DoContinue(world); break;
    case Quit: DoQuit(world); break;
    case Probe: DoProbe(world); break;
    default: break;
    }
    if (!keepalive_.load(std::memory_order_relaxed)) return;
    const ULONGLONG now = GetTickCount64();
    if (now < next_check_) return;
    next_check_ = now + 250;
    bool paused = false;
    if (HubPause::Holding() || !IsPaused(world, paused) || !paused) return;
    const bool ok = SetPaused(world, false);
    if (++lifted_ <= 20 || lifted_ % 100 == 0) Logf("TITLE keepalive lifted a pause ok=%d count=%u", ok ? 1 : 0, lifted_);
}
}
