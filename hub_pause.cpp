#include "hub_pause.h"
#include "settings_store.h"
#include "ue_runtime.h"
#include <atomic>
#include <cstdio>

namespace HubPause {
namespace {
using ue::Object;
using ue::Call;
Logger logger_ = nullptr;
std::atomic<bool> enabled_{true}, open_{false}, holding_{false};
// Game thread only.
Object* engine_ = nullptr;
struct Owned { Object* world = nullptr; ue::Name name; } owned_;
ULONGLONG retry_at_ = 0;
bool refusal_logged_ = false;

void Log(const char* text) { if (logger_) logger_(text); }
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
bool IsPaused(Object* world, bool& value) {
    Call get(Gameplay(), "IsGamePaused");
    if (!get.Slot("ReturnValue", 1) || !get.Set("WorldContextObject", world).Invoke()) return false;
    value = get.Get<bool>("ReturnValue"); return true;
}
bool SetPaused(Object* world, bool value) {
    Call set(Gameplay(), "SetGamePaused");
    return set.Slot("ReturnValue", 1) && set.Set("WorldContextObject", world).Set("bPaused", value).Invoke() && set.Get<bool>("ReturnValue");
}
bool StillOwned(Object* world) {
    return owned_.world && owned_.world == world && ue::Alive(world) && ue::NameOf(world) == owned_.name;
}
void Drop() { owned_ = {}; holding_.store(false, std::memory_order_relaxed); }
}

void Initialize(Logger logger) {
    logger_ = logger;
    enabled_.store(SettingsStore::GetBool(L"Hub", L"PauseGame", true), std::memory_order_relaxed);
}
bool Enabled() { return enabled_.load(std::memory_order_relaxed); }
void SetEnabled(bool enabled) {
    enabled_.store(enabled, std::memory_order_relaxed);
    SettingsStore::PutBool(L"Hub", L"PauseGame", enabled);
}
void Sync(bool open) { open_.store(open, std::memory_order_relaxed); }
bool Holding() { return holding_.load(std::memory_order_relaxed); }
void OnGameThread(bool hold_off) {
    const bool want = open_.load(std::memory_order_relaxed) && enabled_.load(std::memory_order_relaxed) && !hold_off;
    Object* world = CurrentWorld();
    // Level travel replaces the world; the old world's pause went with it.
    if (owned_.world && !StillOwned(world)) { Drop(); Log("HUB_PAUSE world changed; owned pause dropped"); }
    if (!world) return;
    if (want && !owned_.world) {
        const ULONGLONG now = GetTickCount64();
        if (now < retry_at_) return;
        bool paused = false;
        if (!IsPaused(world, paused)) { retry_at_ = now + 1000; return; }
        if (paused) { retry_at_ = now + 250; return; } // Someone else's pause: never claim it.
        if (!SetPaused(world, true) || !IsPaused(world, paused) || !paused) {
            retry_at_ = now + 1000;
            if (!refusal_logged_) { refusal_logged_ = true; Log("HUB_PAUSE SetGamePaused refused (game mode does not allow pausing here); retrying"); }
            return;
        }
        owned_.world = world; owned_.name = ue::NameOf(world); holding_.store(true, std::memory_order_relaxed);
        refusal_logged_ = false; Log("HUB_PAUSE paused while the hub is open");
        return;
    }
    if (!want && owned_.world) {
        bool paused = false;
        // A refused unpause means another pause holder (e.g. the game's own
        // pause menu) now keeps the world paused; leave it to that holder.
        const bool resumed = !IsPaused(world, paused) || !paused || SetPaused(world, false);
        Drop(); retry_at_ = 0; Log(resumed ? "HUB_PAUSE resumed" : "HUB_PAUSE unpause refused; another pause holder keeps the game paused");
    }
}
}
