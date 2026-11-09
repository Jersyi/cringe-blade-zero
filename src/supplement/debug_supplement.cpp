#include "debug_supplement.h"
#include "debug_supplement_catalog.generated.h"
#include "ue_runtime.h"
#include "game_build.h"
#include "sequence_request.h"
#include <windows.h>
#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <deque>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace DebugSupplement {
namespace {
using ue::Object;
using ue::Name;
using ue::Call;
constexpr wchar_t kContainer[] = L"PBZDebugSupplement";
constexpr char kClassPath[] = "/Game/PBZContent/UI/Help/BP_CollectHelp.BP_CollectHelp_C";

// The game mounts only its own paks at startup: its startup mount pass is in protected code and skips every other
// .pak under Paks (PBZDebugSupplement, LogicMods, ~mods alike; the files are never opened). FPakPlatformFile::Mount
// itself is the stock engine one, so the container is mounted at runtime the way HandleMountPaksExDelegate does it:
// FPlatformFileManager::Get().FindPlatformFile(L"PakFile") -> Mount(FPakMountArgs, FIoStatus*, nullptr).
constexpr uintptr_t kFileManagerGetRva = 0x12e0e00, kFindPlatformFileRva = 0x12df700, kPakMountRva = 0x22f8990;
struct EngineCode { uintptr_t rva; unsigned char code[16]; };
constexpr EngineCode kEngineCode[] = {
    {kFileManagerGetRva, {0x48,0x8d,0x05,0x99,0xfd,0xa3,0x0c,0xc3,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc}},
    {kFindPlatformFileRva, {0xe9,0xcb,0x31,0x6a,0x0e,0xcc,0x24,0xdf,0x62,0xbf,0xef,0x03,0xd3,0x20,0xf1,0xe9}},
    {kPakMountRva, {0x40,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x57,0x48,0x8d,0xac,0x24,0x10}},
};
// FPakMountArgs (UE 5.8 IPlatformFilePak.h); the offsets are the ones Mount reads from its second argument.
struct PakMountArgs { const wchar_t* filename = nullptr; uint32_t order = 0; const wchar_t* path = nullptr; uint32_t flags = 0; bool load_index = true; };
static_assert(sizeof(PakMountArgs) == 0x20 && offsetof(PakMountArgs, order) == 0x08 && offsetof(PakMountArgs, path) == 0x10 &&
    offsetof(PakMountArgs, flags) == 0x18 && offsetof(PakMountArgs, load_index) == 0x1c, "FPakMountArgs layout");
// FIoStatus: FSharedString (points at the characters), EIoErrorCode, system error code.
struct IoStatus { const wchar_t* message = nullptr; uint32_t code = 0, system = 0; };
using FileManagerGet = void* (*)();
using FindPlatformFile = void* (*)(void* manager, const wchar_t* name);
using PakMount = bool (*)(void* pak_platform_file, const PakMountArgs* args, void* io_status, void* pak_list_entry);

// FSoftObjectPath inside TSoftClassPtr: weak pointer, package + asset names, sub path.
struct SoftPath { int32_t index = 0, serial = 0; Name package, asset; ue::String sub; };
enum class Action { Load, Call, Probe, Teleport, Reflect };
struct Request { Action action = Action::Load; int32_t item = -1; double x = 0, y = 0, z = 0; };
constexpr size_t kItemCount = std::size(DebugSupplementCatalog::kItems);

SRWLOCK state_lock = SRWLOCK_INIT, queue_lock = SRWLOCK_INIT;
Snapshot state_;
std::deque<Request> queue_;
Logger logger_ = nullptr;
bool present_ = false, mount_tried_ = false, mounted_ = false;
std::wstring pak_path_;
constexpr char kHelpObjClass[] = "/Script/ProjectPBZ.PBZGameplayHelpObj";
Object* engine_ = nullptr, *class_ = nullptr, *instance_ = nullptr, *help_ = nullptr;
int32_t active_item_ = -1;
std::map<std::wstring, Object*> functions_;  // the loaded class chain by name, most derived first
bool available_[kItemCount]{};
uint32_t calls_ = 0;
SupplementSequence::Request sequence_request_;
bool KnownCheat(const char* command);
Object* Instance();

void Log(const char* text) { if (logger_) logger_(text); }
void Logf(const char* format, ...) {
    char line[1024]{};
    va_list arguments; va_start(arguments, format);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, format, arguments);
    va_end(arguments);
    Log(line);
}
std::string Utf8(const std::wstring& text) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}
std::wstring WideName(Name name) { wchar_t text[256]{}; ue::NameWide(name, text, 256); return text; }

bool HasContainer(const std::wstring& directory, const wchar_t* name) {
    for (const wchar_t* extension : {L".pak", L".utoc", L".ucas"})
        if (GetFileAttributesW((directory + name + extension).c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    return true;
}
// Looked up under Paks like the engine's own pak search (mod managers use ~mods\<mod>\).
std::wstring FindContainer(const std::wstring& directory, int depth, const wchar_t* name = kContainer) {
    if (HasContainer(directory, name)) return directory;
    if (depth <= 0) return {};
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW((directory + L"*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return {};
    std::wstring found;
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && wcscmp(data.cFileName, L".") && wcscmp(data.cFileName, L".."))
            found = FindContainer(directory + data.cFileName + L"\\", depth - 1, name);
    } while (found.empty() && FindNextFileW(find, &data));
    FindClose(find);
    return found;
}

void Publish(const wchar_t* status, int32_t finished_item = -1, bool ok = false) {
    AcquireSRWLockExclusive(&state_lock);
    if (finished_item >= 0) {
        const uint64_t sequence = state_.results.empty() ? 1 : state_.results.back().sequence + 1;
        state_.results.push_back({sequence, finished_item, ok, status});
        if (state_.results.size() > 32) state_.results.erase(state_.results.begin());
    }
    state_.present = present_;
    state_.mounted = mounted_;
    state_.loaded = class_ != nullptr;
    state_.calls = calls_;
    state_.status = status;
    for (size_t i = 0; i < kItemCount && i < state_.items.size(); ++i) state_.items[i].available = available_[i];
    ++state_.generation;
    ReleaseSRWLockExclusive(&state_lock);
}

Object* ObjectProperty(Object* object, const char* property) {
    const int32_t offset = object ? ue::Offset(ue::ClassOf(object), property) : -1;
    return offset >= 0 ? ue::At<Object*>(object, offset) : nullptr;
}
Object* CurrentWorld() {
    if (!engine_ || !ue::Alive(engine_)) engine_ = ue::FindEngine();
    Object* world = ObjectProperty(ObjectProperty(engine_, "GameViewport"), "World");
    return ue::Alive(world) ? world : nullptr;
}
Object* GameInstance() {
    Object* world = CurrentWorld();
    Object* instance = world ? ObjectProperty(world, "OwningGameInstance") : nullptr;
    return ue::Alive(instance) ? instance : nullptr;
}

bool EngineCodeMatches() {
    for (const EngineCode& check : kEngineCode)
        if (!GameBuild::CodeMatches(ue::Base() + check.rva, check.code, sizeof(check.code))) return false;
    return true;
}
void CopyText(const wchar_t* text, char* out, size_t size) {
    __try {
        size_t n = 0;
        for (; text && text[n] && n + 1 < size; ++n) out[n] = text[n] < 128 ? static_cast<char>(text[n]) : '?';
        out[n] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { strncpy_s(out, size, "?", _TRUNCATE); }
}
// Once per container and process, on the game thread; the engine keeps a container mounted until exit (the status
// message's shared string is left to the engine allocator: one small block on a failed mount).
bool MountPak(const std::wstring& pak, const char* tag) {
    if (!ue::Base() || !EngineCodeMatches()) { Logf("%s mount skipped: engine code differs from the pinned build", tag); return false; }
    void* manager = reinterpret_cast<FileManagerGet>(ue::Base() + kFileManagerGetRva)();
    void* pak_file = manager ? reinterpret_cast<FindPlatformFile>(ue::Base() + kFindPlatformFileRva)(manager, L"PakFile") : nullptr;
    if (!pak_file) { Logf("%s mount skipped: no PakFile platform layer", tag); return false; }
    PakMountArgs args;
    args.filename = pak.c_str();
    args.order = 4;  // the base game's own order; these packages exist nowhere else
    IoStatus status;
    const bool ok = reinterpret_cast<PakMount>(ue::Base() + kPakMountRva)(pak_file, &args, &status, nullptr);
    char message[256]{};
    CopyText(status.message, message, sizeof(message));
    Logf("%s mount %s ok=%d io=%u system=%u (%s)", tag, Utf8(pak).c_str(), ok ? 1 : 0, status.code, status.system, message);
    return ok;
}
void Mount() {
    mount_tried_ = true;
    mounted_ = MountPak(pak_path_, "SUPPLEMENT");
}

// The class is found if loaded, else loaded blocking; a package that is not mounted returns null.
Object* LoadClass() {
    Object* cls = ue::FindObject(kClassPath);
    if (!cls) {
        SoftPath path;
        path.package = ue::MakeName(L"/Game/PBZContent/UI/Help/BP_CollectHelp");
        path.asset = ue::MakeName(L"BP_CollectHelp_C");
        Call load(ue::Default("/Script/Engine.Default__KismetSystemLibrary"), "LoadClassAsset_Blocking");
        unsigned char* slot = load.Slot("AssetClass", sizeof(path));
        if (!slot) return nullptr;
        std::memcpy(slot, &path, sizeof(path));
        if (load.Invoke()) cls = load.Get<Object*>("ReturnValue");
    }
    return ue::Alive(cls) ? cls : nullptr;
}

// Our counted reference keeps the instance's memory valid even after the game destroys it.
void DropInstance() {
    if (help_) ue::ReleaseReference(help_);
    if (instance_) ue::ReleaseReference(instance_);
    instance_ = help_ = nullptr;
}
Object* Function(const std::wstring& name) {
    const auto it = functions_.find(name);
    return it != functions_.end() ? it->second : nullptr;
}
// Every name an item's code calls on self must resolve: the script VM's FindFunctionChecked is fatal otherwise.
bool ItemAvailable(const DebugSupplementCatalog::Item& item) {
    if (!*item.event) return *item.native && KnownCheat(item.native);  // a DebugObj.js item: the DLL's own code
    if (!Function(item.event)) return false;
    for (const char* name = item.by_name; *name;) {
        const char* end = std::strchr(name, '|');
        const size_t length = end ? static_cast<size_t>(end - name) : std::strlen(name);
        wchar_t wide[128]{};
        if (!MultiByteToWideChar(CP_UTF8, 0, name, static_cast<int>(length), wide, 127) || !Function(wide)) return false;
        name += length + (end ? 1 : 0);
    }
    return true;
}
// Class and function objects are re-read on every request: until the instance exists, the unused Blueprint class is
// collected and reloaded.
bool Prepare() {
    Object* cls = LoadClass();
    if (!cls) {
        DropInstance();
        class_ = nullptr; functions_.clear();
        Log("SUPPLEMENT class not loaded: BP_CollectHelp_C missing from the mounted container");
        return false;
    }
    if (cls != class_ || functions_.empty()) {
        class_ = cls; functions_.clear();
        for (Object* s = cls; s; s = ue::At<Object*>(s, ue::layout::kSuper))
            for (Object* f = ue::At<Object*>(s, ue::layout::kChildren); f; f = ue::At<Object*>(f, ue::layout::kFieldNext))
                functions_.emplace(WideName(ue::NameOf(f)), f);  // emplace keeps the most derived override
        int32_t available = 0;
        for (size_t i = 0; i < kItemCount; ++i) available += (available_[i] = ItemAvailable(DebugSupplementCatalog::kItems[i])) ? 1 : 0;
        Logf("SUPPLEMENT class=%p functions=%zu items=%zu available=%d", static_cast<void*>(cls), functions_.size(), kItemCount, available);
    }
    return true;
}
// The 11-04 game update made the GetWorld of PBZGameplayHelpCollect (and of PBZGameplayHelpObj) the shared
// `return nullptr` stub, so every world-context call in the item code (GetPlayerCharacter, ExecuteConsoleCommand, the UI
// layers) found no world and did nothing. Our two objects get a private copy of their class vtable whose GetWorld slot is
// UObject's own (Outer->GetWorld(): the game instance's world); the classes and every other object keep the game's table.
constexpr size_t kGetWorldSlot = 48;
constexpr uintptr_t kObjectGetWorldRva = 0x15b5e70, kNullStubRva = 0x125a450;
constexpr unsigned char kObjectGetWorldCode[] = {0x48,0x8b,0x49,0x20,0x48,0x85,0xc9,0x74,0x0a,0x48,0x8b,0x01,0x48,0xff,0xa0,0x80,0x01,0x00,0x00};  // exe-bytes: kObjectGetWorldRva
constexpr unsigned char kNullStubCode[] = {0x33,0xc0,0xc3};  // exe-bytes: kNullStubRva
std::map<uintptr_t*, uintptr_t*> world_tables_;  // game vtable -> our copy; never freed (an object may outlive our reset)

bool Code(uintptr_t p) { return p >= ue::Base() && p < ue::Base() + 0x20000000; }
bool GiveWorld(Object* object) {
    const uintptr_t object_get_world = ue::Base() + kObjectGetWorldRva, null_stub = ue::Base() + kNullStubRva;
    if (std::memcmp(reinterpret_cast<const void*>(object_get_world), kObjectGetWorldCode, sizeof(kObjectGetWorldCode)) ||
        std::memcmp(reinterpret_cast<const void*>(null_stub), kNullStubCode, sizeof(kNullStubCode))) return false;
    uintptr_t*& slot = *reinterpret_cast<uintptr_t**>(object);
    uintptr_t* const vtable = slot;
    if (vtable[kGetWorldSlot] == object_get_world) return true;  // our copy already, or a class that resolves the world itself
    if (vtable[kGetWorldSlot] != null_stub) return false;        // an override this build does not have
    const auto known = world_tables_.find(vtable);
    uintptr_t* copy = known != world_tables_.end() ? known->second : nullptr;
    if (!copy) {
        size_t count = 0;
        while (count < 1024 && Code(vtable[count])) ++count;
        if (count <= kGetWorldSlot) return false;
        // vtable[-1] (MSVC complete-object locator) stays in front of the copy, as in the engine tick hook.
        auto* block = static_cast<uintptr_t*>(VirtualAlloc(nullptr, (count + 1) * sizeof(uintptr_t), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!block) return false;
        block[0] = vtable[-1];
        for (size_t i = 0; i < count; ++i) block[i + 1] = vtable[i];
        block[1 + kGetWorldSlot] = object_get_world;
        copy = world_tables_[vtable] = block + 1;
    }
    slot = copy;
    return true;
}

Object* PlayerCharacter(Object* context) {
    Call get(ue::Default("/Script/Engine.Default__GameplayStatics"), "GetPlayerCharacter");
    return get.Set("WorldContextObject", context).Set("PlayerIndex", 0).Invoke() ? get.Get<Object*>("ReturnValue") : nullptr;
}

// The 11-04 game also emptied PBZGameplayHelpObj::TeleportCharacter and ::AddEventTags and
// PBZGameplayHelpCollect::SetDebugFlyGuard (their execs read the argument and call a `ret` stub); the event-NPC items and
// the fly switch call them. Their UFunctions get execs of ours that read the argument the way the game's execs do and do
// what the 10-29 build did: AddEventTags hands the tags to the world's EventPipelineManager (AddTags); the fly guard puts
// the player back into flying, vertical speed 0, whenever the game has let it fall for two ticks; TeleportCharacter
// flies the player there under the guard until ground is found below it (the destination streams in meanwhile). Nothing
// else in the game calls these functions.
using StepFn = void (*)(void* stack, Object* object, void* result);
using StepExplicitFn = void (*)(void* stack, void* result, void* property);
using NativeExec = void (*)(Object* context, unsigned char* stack, void* result);
constexpr uintptr_t kStepRva = 0x1632460, kStepExplicitRva = 0x1632490;  // FFrame::Step, FFrame::StepExplicitProperty
constexpr unsigned char kStepCode[] = {0x48,0x8b,0x41,0x20,0x4c,0x8b,0xd2,0x48,0x8b,0xd1,0x44,0x0f,0xb6,0x08,0x48,0xff};
constexpr unsigned char kStepExplicitCode[] = {0x41,0x8b,0x40,0x38,0x4d,0x8b,0xc8,0x4c,0x8b,0xd1,0x48,0x0f,0xba,0xe0,0x08,0x73};
// FFrame (UE 5.8): Object +0x18, Code +0x20, MostRecentPropertyAddress +0x38, MostRecentPropertyContainer +0x40,
// PropertyChainForCompiledIn +0x88; FProperty::Next +0x10. The offsets the game's own execs use.
void* ReadArgument(unsigned char* stack, void* local) {
    ue::At<void*>(stack, 0x38) = nullptr;
    ue::At<void*>(stack, 0x40) = nullptr;
    if (ue::At<void*>(stack, 0x20)) reinterpret_cast<StepFn>(ue::Base() + kStepRva)(stack, ue::At<Object*>(stack, 0x18), local);
    else {
        void* property = ue::At<void*>(stack, 0x88);
        ue::At<void*>(stack, 0x88) = ue::At<void*>(property, 0x10);
        reinterpret_cast<StepExplicitFn>(ue::Base() + kStepExplicitRva)(stack, local, property);
    }
    void* address = ue::At<void*>(stack, 0x38);
    return address ? address : local;
}
void FinishArguments(unsigned char* stack) {
    unsigned char*& code = ue::At<unsigned char*>(stack, 0x20);
    if (code) ++code;
}

struct Vector { double x, y, z; };
struct TagContainer { ue::Array tags, parents; };  // FGameplayTagContainer
constexpr uint8_t kMoveWalking = 1, kMoveFlying = 5;
// Teleport: the first ground check, the checks after it, and when "no ground yet" is logged; it never drops the player
// from the air (the 11-04 world can leave the destination unstreamed): it flies until ground is found below and is set
// down on it, or until the fly switch is turned off.
constexpr ULONGLONG kLandAfterMs = 2000, kGroundCheckMs = 500, kNoGroundLogMs = 20000;
constexpr double kGroundBelow = 100000, kStandAbove = 10;  // trace depth; clearance over the capsule's half height
// The 10-29 teleport also raised two streaming budgets until it landed (s.AsyncLoadingTimeLimit 500 ms,
// s.LevelStreamingActorsUpdateTimeLimit 5 ms; the game runs them at 0.2), so the destination streams in within seconds.
// Each storage is pinned by the console variable's registration (lea r8, storage).
struct Budget { uintptr_t lea; uintptr_t storage; float teleport; };
constexpr Budget kBudgets[] = {{0xfa6c13, 0xdae8498, 500.f}, {0xfa6f53, 0xdae84a0, 5.f}};  // exe-pins
float saved_budgets_[std::size(kBudgets)]{};
bool budgets_raised_ = false;
float* BudgetStorage(const Budget& budget) {
    const unsigned char* code = reinterpret_cast<const unsigned char*>(ue::Base() + budget.lea);
    if (code[0] != 0x4c || code[1] != 0x8d || code[2] != 0x05) return nullptr;
    int32_t displacement = 0;
    std::memcpy(&displacement, code + 3, sizeof(displacement));
    return ue::Base() + budget.lea + 7 + displacement == ue::Base() + budget.storage ? reinterpret_cast<float*>(ue::Base() + budget.storage) : nullptr;
}
void RaiseBudgets(bool raise) {
    if (raise == budgets_raised_) return;
    for (size_t i = 0; i < std::size(kBudgets); ++i)
        if (float* storage = BudgetStorage(kBudgets[i])) {
            if (raise) { saved_budgets_[i] = *storage; *storage = kBudgets[i].teleport; }
            else *storage = saved_budgets_[i];
        }
    budgets_raised_ = raise;
}
bool fly_guard_ = false;       // the fly switch's guard (SetDebugFlyGuard)
ULONGLONG teleport_at_ = 0;    // a teleport's guard, until it lands
ULONGLONG ground_checked_at_ = 0;
bool no_ground_logged_ = false;
int falling_ticks_ = 0;
bool SetMovementMode(Object* movement, uint8_t mode) {
    Call set(movement, "SetMovementMode");
    return set.Set("NewMovementMode", mode).Set("NewCustomMode", static_cast<uint8_t>(0)).Invoke();
}
Object* PlayerMovement(Object** player_out = nullptr) {
    Object* player = PlayerCharacter(CurrentWorld());
    if (player_out) *player_out = player;
    Object* movement = player ? ObjectProperty(player, "CharacterMovement") : nullptr;
    return ue::Alive(movement) ? movement : nullptr;
}
void StopVelocity(Object* movement) {
    const int32_t velocity = ue::Offset(ue::ClassOf(movement), "Velocity");
    if (velocity >= 0) ue::At<Vector>(movement, velocity) = Vector{};
}
// The first surface under the player (the destination's floor once it has streamed in).
bool GroundBelow(Object* player, Vector& ground) {
    Call where(player, "K2_GetActorLocation");
    if (!where.Invoke()) return false;
    const Vector from = where.Get<Vector>("ReturnValue"), to{from.x, from.y, from.z - kGroundBelow};
    Object* hit_struct = ue::FindObject("/Script/Engine.HitResult");
    const int32_t impact = hit_struct ? ue::Offset(hit_struct, "ImpactPoint") : -1, hit_size = hit_struct ? ue::StructSize(hit_struct) : 0;
    Call trace(ue::Default("/Script/Engine.Default__KismetSystemLibrary"), "LineTraceSingle");
    if (impact < 0 || !trace.Set("WorldContextObject", player).Set("Start", from).Set("End", to).Set("TraceChannel", static_cast<uint8_t>(0))
            .Set("bIgnoreSelf", true).Invoke() || !trace.Get<bool>("ReturnValue")) return false;
    const unsigned char* hit = trace.Slot("OutHit", static_cast<size_t>(hit_size));
    if (!hit) return false;
    ground = ue::At<Vector>(const_cast<unsigned char*>(hit), impact);
    return true;
}
// Stands the player on the ground point (capsule half height above it) and lets it walk.
void Land(Object* player, Object* movement, const Vector& ground) {
    Object* capsule = ObjectProperty(player, "CapsuleComponent");
    const int32_t half = capsule ? ue::Offset(ue::ClassOf(capsule), "CapsuleHalfHeight") : -1;
    const Vector stand{ground.x, ground.y, ground.z + (half >= 0 ? ue::At<float>(capsule, half) : 100.0) + kStandAbove};
    Call(player, "K2_SetActorLocation").Set("NewLocation", stand).Set("bSweep", false).Set("bTeleport", true).Invoke();
    StopVelocity(movement);
    SetMovementMode(movement, kMoveWalking);
}
void EndTeleport() {
    teleport_at_ = 0;
    RaiseBudgets(false);
}
void TeleportPlayer(const Vector& location) {
    Object* player = nullptr;
    Object* movement = PlayerMovement(&player);
    if (!movement) { Log("SUPPLEMENT teleport skipped: no player character"); return; }
    SetMovementMode(movement, kMoveFlying);
    Call move(player, "K2_SetActorLocation");
    const bool moved = move.Set("NewLocation", location).Set("bSweep", false).Set("bTeleport", false).Invoke() && move.Get<bool>("ReturnValue");
    StopVelocity(movement);
    teleport_at_ = GetTickCount64();
    ground_checked_at_ = 0;
    no_ground_logged_ = false;
    falling_ticks_ = 0;
    RaiseBudgets(true);
    Logf("SUPPLEMENT teleport to %.0f %.0f %.0f moved=%d", location.x, location.y, location.z, moved ? 1 : 0);
}
// Every game tick while a guard is on.
void GuardFlight() {
    if (!fly_guard_ && !teleport_at_) return;
    Object* player = nullptr;
    Object* movement = PlayerMovement(&player);
    if (!movement) return;
    const ULONGLONG now = GetTickCount64(), flown = teleport_at_ ? now - teleport_at_ : 0;
    if (teleport_at_ && flown >= kLandAfterMs && now - ground_checked_at_ >= kGroundCheckMs) {
        ground_checked_at_ = now;
        Vector ground{};
        if (GroundBelow(player, ground)) {
            EndTeleport();
            if (!fly_guard_) {
                Land(player, movement, ground);
                Logf("SUPPLEMENT teleport landed after %llu ms on ground at z=%.0f", flown, ground.z);
                return;
            }
        }
        else if (flown >= kNoGroundLogMs && !no_ground_logged_) {
            no_ground_logged_ = true;
            Log("SUPPLEMENT teleport: no ground below after 20 s; still flying (turning flying off ends it)");
        }
    }
    const int32_t mode = ue::Offset(ue::ClassOf(movement), "MovementMode"), velocity = ue::Offset(ue::ClassOf(movement), "Velocity");
    if (mode < 0 || ue::At<uint8_t>(movement, mode) == kMoveFlying) { falling_ticks_ = 0; return; }
    if (++falling_ticks_ < 2) return;
    falling_ticks_ = 0;
    SetMovementMode(movement, kMoveFlying);
    if (velocity >= 0) ue::At<Vector>(movement, velocity).z = 0;
}
// The name of a function's first parameter when it is `bytes` wide (the two help natives take one argument).
bool FirstParameter(Object* function, size_t bytes, char* name, size_t size) {
    void* parameter = function ? ue::At<void*>(function, ue::layout::kChildProperties) : nullptr;
    if (!parameter || ue::At<int32_t>(parameter, ue::layout::kPropElementSize) != static_cast<int32_t>(bytes)) return false;
    ue::NameText(ue::At<Name>(parameter, ue::layout::kPropName), name, size);
    return true;
}
void AddEventTags(const TagContainer& tags) {
    Object* world = CurrentWorld(), *cls = ue::FindObject("/Script/ProjectPBZ.EventPipelineManager");
    Call get(ue::Default("/Script/Engine.Default__SubsystemBlueprintLibrary"), "GetWorldSubsystem");
    Object* manager = world && cls && get.Set("ContextObject", world).Set("Class", cls).Invoke() ? get.Get<Object*>("ReturnValue") : nullptr;
    Object* add = manager ? ue::FindFunction(ue::ClassOf(manager), "AddTags") : nullptr;
    char name[64]{};
    if (!FirstParameter(add, sizeof(TagContainer), name, sizeof(name))) {
        Log("SUPPLEMENT event tags skipped: no EventPipelineManager.AddTags(container)");
        return;
    }
    Call call(manager, add);
    unsigned char* slot = call.Slot(name, sizeof(TagContainer));
    if (slot) std::memcpy(slot, &tags, sizeof(tags));  // the call copies it; the arrays stay the caller's
    Logf("SUPPLEMENT event tags x%d ok=%d", tags.tags.num, slot && call.Invoke() ? 1 : 0);
}
Object* LoadAsset(const wchar_t* package, const wchar_t* asset) {
    SoftPath path;
    path.package = ue::MakeName(package);
    path.asset = ue::MakeName(asset);
    Call load(ue::Default("/Script/Engine.Default__KismetSystemLibrary"), "LoadAsset_Blocking");
    unsigned char* slot = load.Slot("Asset", sizeof(path));
    if (!slot) return nullptr;
    std::memcpy(slot, &path, sizeof(path));
    return load.Invoke() ? load.Get<Object*>("ReturnValue") : nullptr;
}
// The allocated elements of a TSet / TMap: a sparse array {data, num, max} whose allocation bits follow it (4 inline words
// or the secondary block at +0x20, then the bit count at +0x28). -1 when the header does not hold together.
template<class F> int32_t ForEachElement(unsigned char* set, int32_t stride, F&& visit) {
    const ue::Array elements = ue::At<ue::Array>(set, 0);
    if (stride <= 0 || elements.num < 0 || ue::At<int32_t>(set, 0x28) != elements.num || (elements.num && !elements.data)) return -1;
    const uint32_t* bits = ue::At<uint32_t*>(set, 0x20) ? ue::At<uint32_t*>(set, 0x20) : reinterpret_cast<const uint32_t*>(set + 0x10);
    int32_t count = 0;
    for (int32_t i = 0; i < elements.num; ++i)
        if (bits[i >> 5] & (1u << (i & 31))) { visit(static_cast<unsigned char*>(elements.data) + static_cast<size_t>(i) * stride); ++count; }
    return count;
}
// Every row of a data table. RowMap follows the RowStruct property: a TMap of {FName, uint8* row, hash next, hash index}
// (24 bytes).
template<class F> int32_t ForEachRow(const wchar_t* package, const wchar_t* asset, F&& visit) {
    Object* table = LoadAsset(package, asset);
    const int32_t row_struct = table ? ue::Offset(ue::ClassOf(table), "RowStruct") : -1;
    unsigned char* map = row_struct >= 0 ? static_cast<unsigned char*>(table) + row_struct + 8 : nullptr;
    Object* strukt = map ? ue::At<Object*>(table, row_struct) : nullptr;
    int32_t rows = 0;
    if (!map || ForEachElement(map, 24, [&](unsigned char* element) {
            unsigned char* row = ue::At<unsigned char*>(element, 8);
            if (row && visit(strukt, row)) ++rows;
        }) < 0) {
        Logf("SUPPLEMENT table %s not readable", Utf8(asset).c_str());
        return -1;
    }
    return rows;
}
Name ItemId(Object* strukt, unsigned char* row) {
    const int32_t offset = ue::Offset(strukt, "ItemId");
    return offset >= 0 ? ue::At<Name>(row, offset) : Name{};
}
// A game-instance subsystem, else a world subsystem of that class (the JS menu reached both kinds through one helper).
Object* Subsystem(const char* path) {
    Object* cls = ue::FindObject(path), *world = CurrentWorld();
    if (!cls || !world) return nullptr;
    for (const char* getter : {"GetGameInstanceSubsystem", "GetWorldSubsystem"}) {
        Call get(ue::Default("/Script/Engine.Default__SubsystemBlueprintLibrary"), getter);
        Object* found = get.Set("ContextObject", world).Set("Class", cls).Invoke() ? get.Get<Object*>("ReturnValue") : nullptr;
        if (ue::Alive(found)) return found;
    }
    return nullptr;
}
Object* BackPack() { return Subsystem("/Script/ProjectPBZ.PBZBackPackSubsystem"); }

// ---- reflection of the game's containers (the DebugObj.js items read the backpack's maps and data assets) ----
// Property layouts beyond name / size / offset are found at run time and checked; a mismatch refuses the item.
bool Readable(const void* p, size_t bytes) {
    MEMORY_BASIC_INFORMATION info{};
    if (reinterpret_cast<uintptr_t>(p) < 0x10000 || !VirtualQuery(p, &info, sizeof(info)) || info.State != MEM_COMMIT) return false;
    if (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return reinterpret_cast<uintptr_t>(p) + bytes <= reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
}
void* PropertyOf(Object* strukt, const char* name) {
    for (Object* s = strukt; s; s = ue::At<Object*>(s, ue::layout::kSuper))
        for (void* p = ue::At<void*>(s, ue::layout::kChildProperties); p; p = ue::At<void*>(p, ue::layout::kPropNext))
            if (ue::NameIs(ue::At<Name>(p, ue::layout::kPropName), name)) return p;
    return nullptr;
}
int32_t ElementSize(void* property) { return ue::At<int32_t>(property, ue::layout::kPropElementSize); }
int32_t PropertyOffset(void* property) { return ue::At<int32_t>(property, ue::layout::kPropOffset); }
// FField::ClassPrivate (an FFieldClass, its name first): "StructProperty", "MapProperty", ...
bool FieldClassIs(void* property, const char* cls) {
    void* field_class = Readable(property, 0x50) ? ue::At<void*>(property, 0x8) : nullptr;
    if (!Readable(field_class, sizeof(Name))) return false;
    char name[64]{};
    ue::NameText(ue::At<Name>(field_class, 0), name, sizeof(name));
    return cls ? !std::strcmp(name, cls) : std::strstr(name, "Property") != nullptr;
}
// sizeof(FProperty) in this build: an FStructProperty's Struct follows it (CharacterMovementComponent.Velocity -> Vector).
int32_t PropertyBase() {
    static int32_t base = 0;
    if (base) return base;
    Object* movement = ue::FindObject("/Script/Engine.CharacterMovementComponent"), *vector = ue::FindObject("/Script/CoreUObject.Vector");
    void* velocity = movement && vector ? PropertyOf(movement, "Velocity") : nullptr;
    for (int32_t at = 0x60; velocity && at <= 0x98; at += 8)
        if (ue::At<Object*>(velocity, at) == vector) return base = at;
    Log("SUPPLEMENT property layout unknown: no FStructProperty::Struct after FProperty");
    return 0;
}
Object* StructOf(void* property) {
    const int32_t base = PropertyBase();
    return base && FieldClassIs(property, "StructProperty") ? ue::At<Object*>(property, base) : nullptr;
}
// FArrayProperty::Inner follows FProperty, after an EArrayPropertyFlags byte or before it.
void* InnerOf(void* property) {
    const int32_t base = PropertyBase();
    if (!base || !FieldClassIs(property, "ArrayProperty")) return nullptr;
    for (int32_t at : {base, base + 8})
        if (void* inner = ue::At<void*>(property, at); FieldClassIs(inner, nullptr)) return inner;
    return nullptr;
}
// FMapProperty: KeyProp, ValueProp, then FScriptMapLayout {ValueOffset, HashNextIdOffset, HashIndexOffset, Size}.
struct MapShape { int32_t value = 0, stride = 0; void* value_property = nullptr; };
bool ShapeOf(void* property, MapShape& shape) {
    const int32_t base = PropertyBase();
    if (!base || !FieldClassIs(property, "MapProperty")) return false;
    void* key = ue::At<void*>(property, base), *value_property = ue::At<void*>(property, base + 8);
    if (!FieldClassIs(key, nullptr) || !FieldClassIs(value_property, nullptr)) return false;
    const int32_t value = ue::At<int32_t>(property, base + 16), next = ue::At<int32_t>(property, base + 20),
        index = ue::At<int32_t>(property, base + 24), size = ue::At<int32_t>(property, base + 28);
    if (value < ElementSize(key) || next < value + ElementSize(value_property) || index != next + 4 || size < index + 4 || size > index + 16) return false;
    shape = {value, size, value_property};
    return true;
}
// A TMap<FName, V> member: its keys (copied, so the caller may change the map while it acts) and the value struct.
struct MapView { unsigned char* map = nullptr; MapShape shape; Object* value_struct = nullptr; std::vector<Name> keys; };
bool ViewMap(unsigned char* container, Object* strukt, const char* name, MapView& view) {
    void* property = container && strukt ? PropertyOf(strukt, name) : nullptr;
    if (!property || !ShapeOf(property, view.shape)) { Logf("SUPPLEMENT map %s not readable", name); return false; }
    view.map = container + PropertyOffset(property);
    view.value_struct = StructOf(view.shape.value_property);
    view.keys.clear();
    return ForEachElement(view.map, view.shape.stride, [&](unsigned char* element) { view.keys.push_back(ue::At<Name>(element, 0)); }) >= 0;
}
unsigned char* ValueOf(const MapView& view, Name key) {
    unsigned char* found = nullptr;
    ForEachElement(view.map, view.shape.stride, [&](unsigned char* element) { if (!found && ue::At<Name>(element, 0) == key) found = element + view.shape.value; });
    return found;
}
int32_t IntField(Object* strukt, const unsigned char* value, const char* field, int32_t fallback) {
    void* property = strukt && value ? PropertyOf(strukt, field) : nullptr;
    if (!property) return fallback;
    const int32_t size = ElementSize(property);
    return size == 4 ? ue::At<int32_t>(const_cast<unsigned char*>(value), PropertyOffset(property)) : size == 1 ? value[PropertyOffset(property)] : fallback;
}
// Each element of a TArray<struct> member, with the element struct.
template<class F> int32_t ForEachInArray(unsigned char* container, Object* strukt, const char* name, F&& visit) {
    void* property = container && strukt ? PropertyOf(strukt, name) : nullptr;
    Object* element = StructOf(InnerOf(property));
    const int32_t stride = element ? ue::StructSize(element) : 0;
    if (stride <= 0) { Logf("SUPPLEMENT array %s not readable", name); return -1; }
    const ue::Array items = ue::At<ue::Array>(container, PropertyOffset(property));
    for (int32_t i = 0; i < items.num; ++i) visit(static_cast<unsigned char*>(items.data) + static_cast<size_t>(i) * stride, element);
    return items.num;
}
// An out-parameter map the game filled: its element and allocation-bit blocks (the hash block is left; its place in this
// build's TSet is not pinned).
void FreeMap(unsigned char* map) {
    if (void* data = ue::At<void*>(map, 0)) ue::Free(data);
    if (void* bits = ue::At<void*>(map, 0x20)) ue::Free(bits);
}
// An enumerator's value from UEnum::Names (TArray<TPair<FName, int64>> after the CppType string), by the text after "::".
bool EnumValue(const char* path, const char* name, int64_t& value) {
    Object* enumeration = ue::FindObject(path);
    const ue::String cpp = enumeration ? ue::At<ue::String>(enumeration, 0x30) : ue::String{};
    const ue::Array names = enumeration ? ue::At<ue::Array>(enumeration, 0x40) : ue::Array{};
    if (!Readable(cpp.data, 2) || cpp.data[0] != L'E' || names.num <= 0 || names.num > 512 || !Readable(names.data, static_cast<size_t>(names.num) * 16)) return false;
    for (int32_t i = 0; i < names.num; ++i) {
        unsigned char* pair = static_cast<unsigned char*>(names.data) + static_cast<size_t>(i) * 16;
        char text[128]{};
        ue::NameText(ue::At<Name>(pair, 0), text, sizeof(text));
        const char* leaf = std::strstr(text, "::");
        if (!std::strcmp(leaf ? leaf + 2 : text, name)) { value = ue::At<int64_t>(pair, 8); return true; }
    }
    return false;
}

// A call whose arguments follow the parameter order (a native's ChildProperties are its parameters, return value last),
// as the JS menu passed them. Each value must have its parameter's size; integers and texts adapt to the parameter type.
class Invocation {
public:
    Invocation(Object* target, const char* function)
        : function_(target ? ue::FindFunction(ue::ClassOf(target), function) : nullptr), call_(target, function_), name_(function) {}
    template<class T> Invocation& Arg(const T& value) { Put(&value, sizeof(T)); return *this; }
    Invocation& Bytes(const void* data, size_t bytes) { Put(data, bytes); return *this; }
    Invocation& Int(int64_t value) {
        void* p = Parameter(next_);
        const int32_t narrow = static_cast<int32_t>(value);
        if (p && ElementSize(p) == 4) Put(&narrow, 4); else Put(&value, 8);
        return *this;
    }
    Invocation& Text(const wchar_t* text) {  // an FName or FString parameter
        void* p = Parameter(next_);
        if (p && ElementSize(p) == static_cast<int32_t>(sizeof(Name))) { const Name name = ue::MakeName(text); Put(&name, sizeof(name)); return *this; }
        const int32_t n = static_cast<int32_t>(std::wcslen(text)) + 1;
        const ue::String string{text, n, n};  // read by the call; the caller's text outlives it
        Put(&string, sizeof(string));
        return *this;
    }
    bool Invoke() {
        if (!ok_ || !function_) { Logf("SUPPLEMENT call %s refused: %s", name_, function_ ? "parameters differ" : "function not found"); return false; }
        return call_.Invoke();
    }
    unsigned char* Out(int index, size_t bytes) {  // a parameter after the call (out parameters, the return value)
        void* p = Parameter(index);
        char name[96]{};
        if (!p || ElementSize(p) != static_cast<int32_t>(bytes)) return nullptr;
        ue::NameText(ue::At<Name>(p, ue::layout::kPropName), name, sizeof(name));
        return call_.Slot(name, bytes);
    }
    void* Parameter(int index) const {
        void* p = function_ ? ue::At<void*>(function_, ue::layout::kChildProperties) : nullptr;
        for (int i = 0; p && i < index; ++i) p = ue::At<void*>(p, ue::layout::kPropNext);
        return p;
    }
    Object* Function() const { return function_; }
private:
    void Put(const void* data, size_t bytes) {
        unsigned char* slot = nullptr;
        if (void* p = Parameter(next_++); p && ElementSize(p) == static_cast<int32_t>(bytes)) {
            char name[96]{};
            ue::NameText(ue::At<Name>(p, ue::layout::kPropName), name, sizeof(name));
            slot = call_.Slot(name, bytes);
        }
        if (slot) std::memcpy(slot, data, bytes); else ok_ = false;
    }
    Object* function_;
    Call call_;
    const char* name_;
    int next_ = 0;
    bool ok_ = true;
};
Object* Library(const char* cdo) { return ue::Default(cdo); }
constexpr char kPbzLibrary[] = "/Script/ProjectPBZ.Default__PBZBlueprintFunctionLibrary";
void Console(const wchar_t* command) {
    const int32_t n = static_cast<int32_t>(std::wcslen(command)) + 1;
    Call(Library("/Script/Engine.Default__KismetSystemLibrary"), "ExecuteConsoleCommand").Set("WorldContextObject", CurrentWorld())
        .Set("Command", ue::String{command, n, n}).Invoke();
}
// The Blueprint's own print panel, as the JS items printed (its "Push Print Data UI " takes the text).
bool Print(const std::wstring& text) {
    Object* instance = Instance();
    Object* function = Function(L"Push Print Data UI ");
    char name[64]{};
    Logf("SUPPLEMENT print %s", Utf8(text.substr(0, 300)).c_str());
    if (!instance || !FirstParameter(function, sizeof(ue::String), name, sizeof(name))) { Log("SUPPLEMENT print panel unavailable"); return false; }
    const int32_t n = static_cast<int32_t>(text.size()) + 1;
    return Call(instance, function).Set(name, ue::String{text.c_str(), n, n}).Invoke();
}

// PBZCheatManager's GM_ execs are emptied in the 11-04 build; the items ran them as console commands. These do what the
// 10-29 bodies did, through the backpack and event functions the game still has. The count is the rows acted on.
int32_t GiveAllItems(Object* bag, const char*) { return Call(bag, "GetAllItems").Invoke() ? 1 : -1; }
int32_t GiveAllAccessories(Object* bag, const char*) {
    return ForEachRow(L"/Game/PBZContent/DataTable/UI/DT_Accessory", L"DT_Accessory", [&](Object* strukt, unsigned char* row) {
        const Name id = ItemId(strukt, row);
        return !id.None() && Call(bag, "PushAccessoryToInventory").Set("AccessoryID", id).Set("bAutoEquip", true).Invoke();
    });
}
int32_t GiveAllMedicines(Object* bag, const char*) {
    return ForEachRow(L"/Game/PBZContent/DataTable/UI/DT_MedicineTable", L"DT_MedicineTable", [&](Object* strukt, unsigned char* row) {
        const Name id = ItemId(strukt, row);
        return !id.None() && Call(bag, "PushMedicineToInventory").Set("MedicineID", id).Set("bAutoEquip", true).Set("bShowPickupUI", false).Invoke();
    });
}
// Every inventory row of detail type 1 (materials), 10000 each, without the pickup widget.
int32_t FillAllMaterials(Object* bag, const char*) {
    return ForEachRow(L"/Game/PBZContent/DataTable/Item/DT_InventoryData", L"DT_InventoryData", [&](Object* strukt, unsigned char* row) {
        const int32_t type = ue::Offset(strukt, "ItemDetailType");
        const Name id = ItemId(strukt, row);
        return type >= 0 && row[type] == 1 && !id.None() &&
            Call(bag, "AddPropsToBackPack").Set("Num", 10000).Set("ItemId", id).Set("bPushWidget", false).Invoke();
    });
}
// The tag container at row offset 0x630 of both weapon tables, handed to the event pipeline.
int32_t UnlockWeaponTags(Object*, const char*) {
    auto add = [](Object* strukt, unsigned char* row) {
        bool container = false;
        for (Object* s = strukt; s && !container; s = ue::At<Object*>(s, ue::layout::kSuper))
            for (void* p = ue::At<void*>(s, ue::layout::kChildProperties); p && !container; p = ue::At<void*>(p, ue::layout::kPropNext))
                container = ue::At<int32_t>(p, ue::layout::kPropOffset) == 0x630 && ue::At<int32_t>(p, ue::layout::kPropElementSize) == 0x20;
        const TagContainer& tags = ue::At<TagContainer>(row, 0x630);
        if (!container || tags.tags.num <= 0) return false;
        AddEventTags(tags);
        return true;
    };
    const int32_t weapons = ForEachRow(L"/Game/PBZContent/DataTable/UI/DT_WeaponData", L"DT_WeaponData", add);
    const int32_t shaqi = ForEachRow(L"/Game/PBZContent/DataTable/UI/DT_ShaqiWeaponData", L"DT_ShaqiWeaponData", add);
    return weapons < 0 && shaqi < 0 ? -1 : (std::max)(weapons, 0) + (std::max)(shaqi, 0);
}
// ---- the System / Debug items of the removed DebugObj.js (10-29), on the functions the 11-04 game still has ----
// Each returns how many things it acted on, -1 when it could not run. "Normal" weapons are the main weapons, the others
// the Shaqi (killing-intent) weapons, as in the backpack's two maps.
struct WeaponMap { const char* map; const char* level; bool normal; };
constexpr WeaponMap kWeaponMaps[] = {{"MainWeaponInfos", "WeaponLevel", true}, {"ShaqiWeaponInfos", "ShaChiWeaponLevel", false}};
// 武器升级 +N级: every owned weapon of both kinds up N levels (UpgradeWeapon(id, normal, levels)).
int32_t UpgradeWeapons(Object* bag, const char* arg) {
    const int32_t levels = std::atoi(arg);
    int32_t done = 0;
    for (const WeaponMap& kind : kWeaponMaps) {
        MapView weapons;
        if (!ViewMap(bag, ue::ClassOf(bag), kind.map, weapons)) return -1;
        for (const Name id : weapons.keys) done += Invocation(bag, "UpgradeWeapon").Arg(id).Arg(kind.normal).Int(levels).Invoke() ? 1 : 0;
    }
    return done;
}
// 回退武器: every weapon above level 1 back to 1; the materials and dust it returns go back into the backpack.
int32_t ResetWeapons(Object* bag, const char*) {
    int32_t done = 0;
    for (const WeaponMap& kind : kWeaponMaps) {
        MapView weapons;
        if (!ViewMap(bag, ue::ClassOf(bag), kind.map, weapons)) return -1;
        for (const Name id : weapons.keys) {
            if (IntField(weapons.value_struct, ValueOf(weapons, id), kind.level, 2) <= 1) continue;
            Invocation reset(bag, "ResetWeaponToLevel");
            if (!reset.Arg(id).Arg(kind.normal).Int(1).Invoke()) continue;
            unsigned char* refund = reset.Out(3, 0x50);
            unsigned char* dust = reset.Out(4, 4) ? reset.Out(4, 4) : reset.Out(4, 8);
            if (refund && ue::At<ue::Array>(refund, 0).num > 0) Invocation(bag, "AddPropsByMap").Bytes(refund, 0x50).Arg(false).Invoke();
            if (dust && ue::At<int32_t>(dust, 0) > 0) Invocation(bag, "AddDustNum").Int(ue::At<int32_t>(dust, 0)).Invoke();
            if (refund) FreeMap(refund);
            ++done;
        }
    }
    return done;
}
// 全部武器铸魂: every main weapon, and the Shaqi weapons not yet snap-souled.
int32_t SnapSoulWeapons(Object* bag, const char*) {
    int32_t done = 0;
    for (const WeaponMap& kind : kWeaponMaps) {
        MapView weapons;
        if (!ViewMap(bag, ue::ClassOf(bag), kind.map, weapons)) return -1;
        for (const Name id : weapons.keys) {
            if (!kind.normal && IntField(weapons.value_struct, ValueOf(weapons, id), "bAlreadySnapSoul", 0)) continue;
            done += Invocation(bag, "SnapSoulWeapon").Arg(id).Arg(kind.normal).Invoke() ? 1 : 0;
        }
    }
    return done;
}
// 所有药瓶和饰品升满级: owned medicines and accessories to their maximum through the upgrade screen's functions, without
// cost; a refundable bonus accessory the game refuses to refine stays as it is ("未升满").
int32_t MaxMedicinesAccessories(Object* bag, const char*) {
    struct Kind { const char* map; const char* max; const char* upgrade; const wchar_t* label; };
    constexpr Kind kKinds[] = {{"MedicineInfos", "GetMedicineMaxLevel", "UpgradeMedicine", L"药瓶"},
                               {"Accessories", "GetAccessoryMaxRefineLevel", "UpgradeAccessory", L"饰品"}};
    std::wstring text;
    int32_t done = 0;
    for (const Kind& kind : kKinds) {
        MapView owned;
        if (!ViewMap(bag, ue::ClassOf(bag), kind.map, owned)) return -1;
        int32_t maxed = 0;
        for (const Name id : owned.keys) {
            const int32_t before = IntField(owned.value_struct, ValueOf(owned, id), "CurrentLevel", -1);
            Invocation max(bag, kind.max);
            const unsigned char* top = max.Arg(id).Invoke() ? max.Out(1, 4) : nullptr;
            const int32_t limit = top ? ue::At<int32_t>(const_cast<unsigned char*>(top), 0) : -1;
            if (before >= 0 && limit > before) Invocation(bag, kind.upgrade).Arg(id).Int(limit - before).Invoke();
            const int32_t after = IntField(owned.value_struct, ValueOf(owned, id), "CurrentLevel", -1);
            maxed += after >= limit && limit >= 0 ? 1 : 0;
            wchar_t name[128]{}, line[256]{};
            ue::NameWide(id, name, _countof(name));
            _snwprintf_s(line, _countof(line), _TRUNCATE, L"[%ls] %ls: %d -> %d / %d%ls\n", kind.label, name, before, after, limit,
                after < limit ? L"（未升满）" : L"");
            text += line;
        }
        wchar_t summary[128]{};
        _snwprintf_s(summary, _countof(summary), _TRUNCATE, L"%ls已满级 %d/%zu\n", kind.label, maxed, owned.keys.size());
        text = summary + text;
        done += static_cast<int32_t>(owned.keys.size());
    }
    Print(text);
    return done;
}
void AddProps(Object* bag, const wchar_t* id, int32_t count, bool widget) {
    Call(bag, "AddPropsToBackPack").Set("Num", count).Set("ItemId", ue::MakeName(id)).Set("bPushWidget", widget).Invoke();
}
// 获取所有药瓶并获取升级材料: every medicine of DT_MedicineTable initialised, plus the dust and materials all their level-ups
// consume (MedicineLevelData[].ConsumeDust / ConsumeMaterials).
int32_t MedicinesWithMaterials(Object* bag, const char*) {
    std::vector<Name> ids;
    std::vector<std::pair<Name, int64_t>> materials;
    int64_t dust = 0;
    bool readable = true;
    const int32_t rows = ForEachRow(L"/Game/PBZContent/DataTable/UI/DT_MedicineTable", L"DT_MedicineTable", [&](Object* strukt, unsigned char* row) {
        const Name id = ItemId(strukt, row);
        if (id.None()) return false;
        ids.push_back(id);
        readable &= ForEachInArray(row, strukt, "MedicineLevelData", [&](unsigned char* level, Object* level_struct) {
            dust += IntField(level_struct, level, "ConsumeDust", 0);
            MapView consume;
            if (!ViewMap(level, level_struct, "ConsumeMaterials", consume)) return;
            for (const Name material : consume.keys) {
                const int32_t count = ue::At<int32_t>(ValueOf(consume, material), 0);
                auto it = std::find_if(materials.begin(), materials.end(), [&](const auto& m) { return m.first == material; });
                if (it == materials.end()) materials.push_back({material, count}); else it->second += count;
            }
        }) >= 0;
        return true;
    });
    if (rows < 0 || !readable) return -1;
    const ue::Array list{ids.data(), static_cast<int32_t>(ids.size()), static_cast<int32_t>(ids.size())};  // read by the call
    Invocation(bag, "InitMedicines").Arg(list).Invoke();
    for (const auto& [material, count] : materials)
        Call(bag, "AddPropsToBackPack").Set("Num", static_cast<int32_t>(count)).Set("ItemId", material).Set("bPushWidget", false).Invoke();
    if (dust > 0) Invocation(bag, "AddDustNum").Int(dust).Invoke();
    Logf("SUPPLEMENT medicines %d, materials %zu kinds, dust %lld", rows, materials.size(), dust);
    return rows;
}
// 武器升级测试所需材料: all weapons, their equip conditions, every material, 100 weapon-return items, a million dust, every
// accessory and medicine.
int32_t WeaponTestKit(Object* bag, const char*) {
    const bool weapons = Call(bag, "GetAllWeaponsByDataTable").Invoke();
    UnlockWeaponTags(bag, "");
    FillAllMaterials(bag, "");
    AddProps(bag, L"Return_Weapon_1", 100, false);
    Invocation(bag, "AddDustNum").Int(1000000).Invoke();
    GiveAllAccessories(bag, "");
    GiveAllMedicines(bag, "");
    return weapons ? 1 : -1;
}
int32_t BlackstoneAmber(Object* bag, const char* arg) {
    const int32_t count = std::atoi(arg);
    AddProps(bag, L"Item_001", count, false);  // black stone
    AddProps(bag, L"Item_002", count, false);  // amber
    return count;
}
int32_t Traversing(Object* bag, const char*) { AddProps(bag, L"Item_Traversing", 100, false); return 100; }
// 解锁6个饰品槽: the four slot talents through the ability-node component (nodes already active are skipped by it). Its
// ActivateNode parameters are filled by type: the node name, the player's ability system, false for the flags.
int32_t SlotTalents(Object*, const char*) {
    Object* world = CurrentWorld(), *player = PlayerCharacter(world);
    Invocation get(Library(kPbzLibrary), "GetPBZAbilityNodesComponent");
    unsigned char* found = get.Arg(static_cast<Object*>(player ? player : world)).Invoke() ? get.Out(1, sizeof(Object*)) : nullptr;
    Object* nodes = found ? ue::At<Object*>(found, 0) : nullptr;
    Call asc_get(Library("/Script/GameplayAbilities.Default__AbilitySystemBlueprintLibrary"), "GetAbilitySystemComponent");
    Object* asc = player && asc_get.Set("Actor", player).Invoke() ? asc_get.Get<Object*>("ReturnValue") : nullptr;
    if (!ue::Alive(nodes)) return -1;
    int32_t done = 0;
    for (const wchar_t* node : {L"HeiShi2", L"MemoryFlower4", L"ShaChiHuPo5", L"TopRongShuZhiXin4"}) {
        Invocation activate(nodes, "ActivateNode");
        const Name id = ue::MakeName(node);
        const bool off = false;
        for (int i = 0; void* p = activate.Parameter(i); ++i) {
            if (ue::NameIs(ue::At<Name>(p, ue::layout::kPropName), "ReturnValue")) break;
            if (FieldClassIs(p, "NameProperty")) activate.Arg(id);
            else if (FieldClassIs(p, "ObjectProperty")) activate.Arg(asc);
            else if (FieldClassIs(p, "BoolProperty")) activate.Arg(off);
            else { activate.Bytes(nullptr, 0); break; }  // an unexpected parameter refuses the call
        }
        done += activate.Invoke() ? 1 : 0;
    }
    return done;
}
// 获取前三关所有收集物: everything DT_MapItemData lists for the maps xigu / fujizhilin / lujiacun (weapons, Shaqi weapons, accessories,
// medicines, memory flowers and the map's debug item list), with or without the pickup screens.
int32_t FirstMapsCollectibles(Object* bag, const char* arg) {
    const bool show = std::atoi(arg) != 0;
    Object* data = LoadAsset(L"/Game/PBZContent/Maps/DT_MapItemData", L"DT_MapItemData");
    MapView maps, debug_items;
    if (!data || !ViewMap(data, ue::ClassOf(data), "MapItemDatas", maps)) return -1;
    const bool has_debug = ViewMap(data, ue::ClassOf(data), "DebugAllItems", debug_items);
    std::vector<std::pair<Name, int32_t>> props;
    auto add_prop = [&](Name id) {
        if (id.None()) return;
        auto it = std::find_if(props.begin(), props.end(), [&](const auto& p) { return p.first == id; });
        if (it == props.end()) props.push_back({id, 1}); else ++it->second;
    };
    auto name_field = [](Object* strukt, unsigned char* element, const char* field) {
        void* p = PropertyOf(strukt, field);
        return p ? ue::At<Name>(element, PropertyOffset(p)) : Name{};
    };
    int32_t done = 0;
    for (const wchar_t* map_id : {L"xigu", L"fujizhilin", L"lujiacun"}) {
        const Name key = ue::MakeName(map_id);
        unsigned char* items = ValueOf(maps, key);
        if (!items || !maps.value_struct) { Logf("SUPPLEMENT map items %s missing", Utf8(map_id).c_str()); continue; }
        if (unsigned char* list = has_debug ? ValueOf(debug_items, key) : nullptr) {
            void* ids = debug_items.value_struct ? PropertyOf(debug_items.value_struct, "ItemIds") : nullptr;
            const ue::Array array = ids ? ue::At<ue::Array>(list, PropertyOffset(ids)) : ue::Array{};
            for (int32_t i = 0; i < array.num; ++i) add_prop(static_cast<Name*>(array.data)[i]);
        }
        ForEachInArray(items, maps.value_struct, "MemoryFlowerData", [&](unsigned char* e, Object* s) { add_prop(name_field(s, e, "ItemID")); });
        for (const auto& [field, normal] : {std::pair{"WeaponItemDatas", true}, std::pair{"ShaChiWeaponItemDatas", false}})
            ForEachInArray(items, maps.value_struct, field, [&](unsigned char* e, Object* s) {
                done += Invocation(bag, "AddWeaponItem").Arg(name_field(s, e, "WeaponID")).Arg(normal).Arg(show).Arg(false).Arg(false).Invoke() ? 1 : 0;
            });
        ForEachInArray(items, maps.value_struct, "AccessoryItemDatas", [&](unsigned char* e, Object* s) {
            done += Call(bag, "PushAccessoryToInventory").Set("AccessoryID", name_field(s, e, "AccessoryID")).Set("bAutoEquip", !show).Invoke() ? 1 : 0;
        });
        ForEachInArray(items, maps.value_struct, "MedicineItemDatas", [&](unsigned char* e, Object* s) {
            done += Call(bag, "PushMedicineToInventory").Set("MedicineID", name_field(s, e, "MedicineID")).Set("bAutoEquip", !show)
                .Set("bShowPickupUI", show).Invoke() ? 1 : 0;
        });
    }
    for (const auto& [id, count] : props)
        Call(bag, "AddPropsToBackPack").Set("Num", count).Set("ItemId", id).Set("bPushWidget", show).Invoke();
    return done + static_cast<int32_t>(props.size());
}
int32_t ShopTags(Object*, const char*) {
    Object* shop = Subsystem("/Script/ProjectPBZ.ShopSystem");
    return shop && Call(shop, "AddAllShopTags").Invoke() ? 1 : -1;
}
// 直接从溪谷开始新游戏: the start menu's New Game into 溪谷 without the prologue. A new save: StartNewGame resets the whole
// save object (the old save file is replaced at the new one's first save). The current difficulty is kept; Nightmare
// takes a free Nightmare slot as the start menu does.
int32_t NewGameAtXigu(Object*, const char*) {
    Object* world = CurrentWorld(), *game = GameInstance();
    if (!world || !game) return -1;
    Invocation difficulty_get(Library(kPbzLibrary), "GetDiffcultMode");
    const unsigned char* difficulty_out = difficulty_get.Arg(world).Invoke() ? difficulty_get.Out(1, 1) : nullptr;
    const uint8_t difficulty = difficulty_out ? *difficulty_out : 0;
    Call(game, "EndTutorial").Invoke();
    if (!Call(game, "StartNewGame").Invoke()) return -1;
    if (Object* save = Subsystem("/Script/ProjectPBZ.PBZSaveLoadSubsystem")) {
        int64_t nightmare = -1;
        if (EnumValue("/Script/ProjectPBZ.EDifficultMode", "NightMare", nightmare) && difficulty == nightmare) Call(save, "AllocateFreeNightmareSlot").Invoke();
        Invocation(save, "SetSaveGameDifficultyMode").Arg(difficulty).Invoke();
    }
    Invocation policy_get(Library(kPbzLibrary), "GetCorrectPolicyInStartMenu");
    unsigned char* policy_out = policy_get.Arg(world).Invoke() ? policy_get.Out(1, sizeof(Object*)) : nullptr;
    Object* policy = policy_out ? ue::At<Object*>(policy_out, 0) : nullptr;
    const int32_t option_at = ue::Alive(policy) ? ue::Offset(ue::ClassOf(policy), "OpenLevelOption") : -1;
    const ue::String option = option_at >= 0 ? ue::At<ue::String>(policy, option_at) : ue::String{L"", 1, 1};
    Invocation(game, "SetLoadingScreen").Text(L"PBZ_Xigu_WP").Invoke();
    Logf("SUPPLEMENT new game at PBZ_Xigu_WP difficulty=%u", difficulty);
    return Call(Library("/Script/Engine.Default__GameplayStatics"), "OpenLevel").Set("WorldContextObject", world)
        .Set("LevelName", ue::MakeName(L"PBZ_Xigu_WP")).Set("bAbsolute", true).Set("Options", option.data ? option : ue::String{L"", 1, 1}).Invoke() ? 1 : -1;
}
// 清空残破铃铛状态: out of every nested broken-bell state, so the teleport gate no longer blocks.
int32_t ClearBrokenBells(Object*, const char*) {
    Object* bells = Subsystem("/Script/ProjectPBZ.BellManager");
    return bells && Call(bells, "ClearAllBrokenBellStates").Invoke() ? 1 : -1;
}
int32_t DeactivateBell(Object*, const char* arg) {
    Object* bells = Subsystem("/Script/ProjectPBZ.BellManager");
    wchar_t id[64]{};
    MultiByteToWideChar(CP_UTF8, 0, arg, -1, id, 63);
    return bells && Invocation(bells, "DeactivateBell").Text(id).Invoke() ? 1 : -1;
}
// 交互Debug: the interaction system's debug switches on (and its two candidate checks off), or back to the defaults.
int32_t InteractDebug(Object*, const char* arg) {
    const bool on = std::atoi(arg) != 0;
    for (const wchar_t* name : {L"PBZ.Interact.DebugLog", L"PBZ.Interact.ShowStateOverlay", L"pbz.DebugGrassInteractor"})
        Console((std::wstring(name) + (on ? L" 1" : L" 0")).c_str());
    for (const wchar_t* name : {L"PBZ.Interact.IsNeedCheckNavMesh", L"PBZ.Interact.IsNeedCheckShaChiWeapon"})
        Console((std::wstring(name) + (on ? L" 0" : L" 1")).c_str());
    return 5;
}
Object* StaticObject(const char* cdo, const char* function) {
    Invocation get(Library(cdo), function);
    unsigned char* out = get.Arg(CurrentWorld()).Invoke() ? get.Out(1, sizeof(Object*)) : nullptr;
    Object* found = out ? ue::At<Object*>(out, 0) : nullptr;
    return ue::Alive(found) ? found : nullptr;
}
int32_t InteractingActor(Object*, const char*) {
    Object* component = StaticObject("/Script/ProjectPBZ.Default__PBZInteractComponent", "GetInteractComponent");
    if (!component) return -1;
    Object* actor = ObjectProperty(component, "InteractingActor");
    Print(ue::Alive(actor) ? L"Find InteractingActor " + WideName(ue::NameOf(actor)) : std::wstring(L"Find InteractingActor: none"));
    return actor ? 1 : 0;
}
// 打印当前两把MainWeapon战斗属性: GetWeaponCombatAttribute(id, true, level) of each equipped main weapon.
int32_t WeaponAttributes(Object* bag, const char*) {
    const int32_t at = ue::Offset(ue::ClassOf(bag), "MainWeapons");
    MapView infos;
    if (at < 0 || !ViewMap(bag, ue::ClassOf(bag), "MainWeaponInfos", infos)) return -1;
    const ue::Array equipped = ue::At<ue::Array>(bag, at);
    std::wstring text;
    for (int32_t i = 0; i < equipped.num; ++i) {
        const Name id = static_cast<Name*>(equipped.data)[i];
        const int32_t level = IntField(infos.value_struct, ValueOf(infos, id), "WeaponLevel", 1);
        wchar_t name[128]{}, line[256]{};
        ue::NameWide(id, name, _countof(name));
        _snwprintf_s(line, _countof(line), _TRUNCATE, L"--- 武器[%d] %ls (Lv %d) ---\n", i, name, level);
        text += line;
        Invocation attributes(bag, "GetWeaponCombatAttribute");
        void* result = attributes.Parameter(3);
        MapShape shape;
        unsigned char* map = attributes.Arg(id).Arg(true).Int(level).Invoke() && result && ShapeOf(result, shape) ? attributes.Out(3, 0x50) : nullptr;
        if (!map) { text += L"    (无战斗属性)\n"; continue; }
        ForEachElement(map, shape.stride, [&](unsigned char* element) {
            wchar_t tag[160]{};
            ue::NameWide(ue::At<Name>(element, 0), tag, _countof(tag));
            _snwprintf_s(line, _countof(line), _TRUNCATE, L"    %ls: %g\n", tag, ue::At<float>(element, shape.value));
            text += line;
        });
        FreeMap(map);
    }
    if (!equipped.num) text = L"身上没有 MainWeapon";
    Print(text);
    return equipped.num;
}
// 魂的发色肤色: the appearance component's skin colour (Hun's hair / body grey scale).
int32_t SkinColor(Object*, const char* arg) {
    Object* appearance = StaticObject("/Script/ProjectPBZ.Default__PBZAppearanceComponent", "GetPBZAppearanceComponent");
    int64_t value = 0;
    if (!appearance || !EnumValue("/Script/ProjectPBZ.EPBZSkinColor", arg, value)) return -1;
    return Invocation(appearance, "SetSkinColor").Arg(static_cast<uint8_t>(value)).Invoke() ? 1 : -1;
}

struct Cheat { const char* command; bool backpack; int32_t (*run)(Object* bag, const char* arg); };
constexpr Cheat kCheats[] = {
    {"GM_GetAllItems", true, &GiveAllItems}, {"GM_GetAllAccessories", true, &GiveAllAccessories}, {"GetAllMedicines", true, &GiveAllMedicines},
    {"GM_FillAllMaterilas", true, &FillAllMaterials}, {"GM_GetAllWeaponLockTags", true, &UnlockWeaponTags},
    {"UpgradeWeapons", true, &UpgradeWeapons}, {"ResetWeapons", true, &ResetWeapons}, {"SnapSoulWeapons", true, &SnapSoulWeapons},
    {"MaxMedicinesAccessories", true, &MaxMedicinesAccessories}, {"MedicinesWithMaterials", true, &MedicinesWithMaterials},
    {"WeaponTestKit", true, &WeaponTestKit}, {"BlackstoneAmber", true, &BlackstoneAmber}, {"Traversing", true, &Traversing},
    {"SlotTalents", false, &SlotTalents}, {"FirstMapsCollectibles", true, &FirstMapsCollectibles}, {"ShopTags", false, &ShopTags},
    {"NewGameAtXigu", false, &NewGameAtXigu}, {"ClearBrokenBells", false, &ClearBrokenBells}, {"DeactivateBell", false, &DeactivateBell},
    {"InteractDebug", false, &InteractDebug}, {"InteractingActor", false, &InteractingActor}, {"WeaponAttributes", true, &WeaponAttributes},
    {"SkinColor", false, &SkinColor},
};
// "Name argument": the cheat and the text after its first space.
const Cheat* FindCheat(const char* command, const char** argument = nullptr) {
    const char* space = std::strchr(command, ' ');
    const size_t length = space ? static_cast<size_t>(space - command) : std::strlen(command);
    if (argument) *argument = space ? space + 1 : "";
    for (const Cheat& cheat : kCheats)
        if (std::strlen(cheat.command) == length && !std::strncmp(cheat.command, command, length)) return &cheat;
    return nullptr;
}
bool KnownCheat(const char* command) { return FindCheat(command) != nullptr; }
bool RunCheat(const char* command) {
    const char* argument = "";
    const Cheat* cheat = FindCheat(command, &argument);
    if (!cheat) { Logf("SUPPLEMENT cheat %s unknown", command); return false; }
    Object* bag = cheat->backpack ? BackPack() : nullptr;
    const int32_t rows = !cheat->backpack || bag ? cheat->run(bag, argument) : -1;
    Logf("SUPPLEMENT cheat %s backpack=%p rows=%d", command, static_cast<void*>(bag), rows);
    return rows >= 0;
}

// PBZGameplayHelpCollect's text natives return an FString the item code shows in its print panel.
void SetResultString(void* result, const std::wstring& text) {
    const int32_t length = static_cast<int32_t>(text.size()) + 1;
    const ue::String a{text.c_str(), length, length}, b{L"", 1, 1};
    Call concat(ue::Default("/Script/Engine.Default__KismetStringLibrary"), "Concat_StrStr");  // an engine-owned copy
    if (result && concat.Set("A", a).Set("B", b).Invoke()) *static_cast<ue::String*>(result) = concat.Get<ue::String>("ReturnValue");
}
// "%s : %f" per attribute of the player's ability system, as the 10-29 GetAllAttributeValue printed them.
std::wstring AttributeText() {
    Object* player = PlayerCharacter(CurrentWorld());
    Call get(ue::Default("/Script/GameplayAbilities.Default__AbilitySystemBlueprintLibrary"), "GetAbilitySystemComponent");
    Object* asc = player && get.Set("Actor", player).Invoke() ? get.Get<Object*>("ReturnValue") : nullptr;
    if (!asc) return L"AbilitySystemComponent not found";
    Call all(asc, "GetAllAttributes");
    if (!all.Invoke()) return L"";
    const ue::Array attributes = all.Get<ue::Array>("OutAttributes");
    constexpr size_t kAttribute = 0x38;  // FGameplayAttribute: AttributeName (FString) first
    std::wstring text;
    for (int32_t i = 0; i < attributes.num; ++i) {
        unsigned char* attribute = static_cast<unsigned char*>(attributes.data) + i * kAttribute;
        Call value(ue::Default("/Script/GameplayAbilities.Default__AbilitySystemBlueprintLibrary"), "GetFloatAttributeFromAbilitySystemComponent");
        unsigned char* slot = value.Slot("Attribute", kAttribute);
        if (slot) std::memcpy(slot, attribute, kAttribute);
        const float number = slot && value.Set("AbilitySystem", asc).Invoke() ? value.Get<float>("ReturnValue") : 0.f;
        const ue::String name = ue::At<ue::String>(attribute, 0);
        wchar_t line[256]{};
        _snwprintf_s(line, _countof(line), _TRUNCATE, L"%ls : %f\n", name.data && name.num > 1 ? name.data : L"none", number);
        text += line;
    }
    for (int32_t i = 0; i < attributes.num; ++i)
        if (void* name = ue::At<void*>(static_cast<unsigned char*>(attributes.data) + i * kAttribute, 0)) ue::Free(name);
    if (attributes.data) ue::Free(attributes.data);
    return text;
}
// "GridName : %s, LoadingRange : %d" per runtime streaming grid of the world partition, as the 10-29 build printed them.
std::wstring GridText() {
    Object* settings = ObjectProperty(ObjectProperty(CurrentWorld(), "PersistentLevel"), "WorldSettings");
    Object* hash = ObjectProperty(ObjectProperty(settings, "WorldPartition"), "RuntimeHash");
    const int32_t data = hash ? ue::Offset(ue::ClassOf(hash), "RuntimeStreamingData") : -1;
    Object* element = ue::FindObject("/Script/Engine.RuntimePartitionStreamingData");
    const int32_t stride = element ? ue::StructSize(element) : 0, name = element ? ue::Offset(element, "Name") : -1,
        range = element ? ue::Offset(element, "LoadingRange") : -1;
    if (data < 0 || stride <= 0 || name < 0 || range < 0) return L"No runtime streaming data";
    const ue::Array grids = ue::At<ue::Array>(hash, data);
    std::wstring text;
    for (int32_t i = 0; i < grids.num; ++i) {
        unsigned char* grid = static_cast<unsigned char*>(grids.data) + static_cast<size_t>(i) * stride;
        wchar_t grid_name[128]{}, line[256]{};
        ue::NameWide(ue::At<Name>(grid, name), grid_name, _countof(grid_name));
        _snwprintf_s(line, _countof(line), _TRUNCATE, L"GridName : %ls, LoadingRange : %d\n", grid_name, ue::At<int32_t>(grid, range));
        text += line;
    }
    return text;
}
// RunTrace / RunAutomatedPerfTest: the engine subsystem PBZTraceSystem still has both bodies.
void TraceSystem(const char* function) {
    Object* cls = ue::FindObject("/Script/ProjectPBZ.PBZTraceSystem");
    Call get(ue::Default("/Script/Engine.Default__SubsystemBlueprintLibrary"), "GetEngineSubsystem");
    Object* system = cls && get.Set("Class", cls).Invoke() ? get.Get<Object*>("ReturnValue") : nullptr;
    const bool ok = system && Call(system, function).Invoke();
    Logf("SUPPLEMENT trace %s system=%p ok=%d", function, static_cast<void*>(system), ok ? 1 : 0);
}
void ExecGetAllAttributeValue(Object*, unsigned char* stack, void* result) { FinishArguments(stack); SetResultString(result, AttributeText()); }
void ExecGetRuntimeGridDebugName(Object*, unsigned char* stack, void* result) { FinishArguments(stack); SetResultString(result, GridText()); }
void ExecRunTrace(Object*, unsigned char* stack, void*) { FinishArguments(stack); TraceSystem("ExecuteTraceLogic"); }
void ExecRunAutomatedPerfTest(Object*, unsigned char* stack, void*) { FinishArguments(stack); TraceSystem("ExecuteAutomatedCameraLogic"); }

void ExecTeleportCharacter(Object*, unsigned char* stack, void*) {
    Vector local{};
    const Vector location = *static_cast<const Vector*>(ReadArgument(stack, &local));
    FinishArguments(stack);
    TeleportPlayer(location);
}
void ExecSetDebugFlyGuard(Object*, unsigned char* stack, void*) {
    uint32_t local = 0;  // a bool argument is stepped into a 32-bit local, as P_GET_UBOOL does
    ReadArgument(stack, &local);
    FinishArguments(stack);
    fly_guard_ = local != 0;
    falling_ticks_ = 0;
    if (!fly_guard_) EndTeleport();  // the fly switch's off also ends a teleport that is still looking for ground
    Logf("SUPPLEMENT fly guard %s", fly_guard_ ? "on" : "off");
}
void ExecAddEventTags(Object*, unsigned char* stack, void*) {
    TagContainer local{};
    const TagContainer* tags = static_cast<const TagContainer*>(ReadArgument(stack, &local));
    FinishArguments(stack);
    AddEventTags(*tags);
    if (local.tags.data) ue::Free(local.tags.data);
    if (local.parents.data) ue::Free(local.parents.data);
}
struct ExecPatch { const char* cls; const char* function; uintptr_t game_exec, stub_call; unsigned char call[5]; NativeExec ours; };
// game_exec: the emptied exec; stub_call: the instruction that pins it as the emptied one (its call of the `ret` stub
// 0x125a3e0 or of the FString{} stub 0x12b2470, or the shared empty exec's jump into the protector, 0x16963980 on Steam;
// the jump differs per build, game_build.h).
constexpr char kHelpCollectClass[] = "/Script/ProjectPBZ.PBZGameplayHelpCollect";
constexpr ExecPatch kHelpExecs[] = {  // exe-pins
    {kHelpObjClass, "TeleportCharacter", 0x6591090, 0x6591109, {0xe8,0xd2,0x92,0xcc,0xfa}, &ExecTeleportCharacter},
    {kHelpObjClass, "AddEventTags", 0x658ea20, 0x658eaa8, {0xe8,0x33,0xb9,0xcc,0xfa}, &ExecAddEventTags},
    {kHelpCollectClass, "SetDebugFlyGuard", 0x6569fb0, 0x656a022, {0xe8,0xb9,0x03,0xcf,0xfa}, &ExecSetDebugFlyGuard},
    {kHelpCollectClass, "GetAllAttributeValue", 0x33338e0, 0x3333905, {0xe8,0x66,0xeb,0xf7,0xfd}, &ExecGetAllAttributeValue},
    {kHelpCollectClass, "GetRuntimeGridDebugName", 0x33338e0, 0x3333905, {0xe8,0x66,0xeb,0xf7,0xfd}, &ExecGetRuntimeGridDebugName},
    {kHelpCollectClass, "RunTrace", 0x57f0870, 0x57f0870, {0xe9,0x0b,0x31,0x17,0x11}, &ExecRunTrace},
    {kHelpCollectClass, "RunAutomatedPerfTest", 0x57f0870, 0x57f0870, {0xe9,0x0b,0x31,0x17,0x11}, &ExecRunAutomatedPerfTest},
};
// UFunction::Func is found by its value (the game's exec) after the UStruct part.
bool PatchExec(const ExecPatch& patch) {
    Object* cls = ue::FindObject(patch.cls);
    Object* function = cls ? ue::FindFunction(cls, patch.function) : nullptr;
    if (!function || !GameBuild::CodeMatches(ue::Base() + patch.stub_call, patch.call, sizeof(patch.call))) return false;
    const uintptr_t game = ue::Base() + patch.game_exec, ours = reinterpret_cast<uintptr_t>(patch.ours);
    for (int32_t offset = 0xb0; offset < 0x140; offset += 8) {
        uintptr_t& slot = ue::At<uintptr_t>(function, offset);
        if (slot == ours) return true;
        if (slot == game) { slot = ours; return true; }
    }
    return false;
}
void RestoreHelpExecs() {
    static bool done = false;
    if (done) return;
    done = true;
    const bool steps = !std::memcmp(reinterpret_cast<const void*>(ue::Base() + kStepRva), kStepCode, sizeof(kStepCode)) &&
        !std::memcmp(reinterpret_cast<const void*>(ue::Base() + kStepExplicitRva), kStepExplicitCode, sizeof(kStepExplicitCode));
    std::string restored;
    for (const ExecPatch& patch : kHelpExecs)
        restored += std::string(" ") + patch.function + "=" + (steps && PatchExec(patch) ? "1" : "0");
    Logf("SUPPLEMENT help natives restored:%s (frame steps match=%d)", restored.c_str(), steps ? 1 : 0);
}

// One referenced instance in the game instance (it survives map changes and GC, and with it its class and the
// Blueprint's latent actions); its Initialize runs the Collect functions once, which set up the state the item events read.
Object* Instance() {
    if (instance_ && ue::Alive(instance_) && ue::ClassOf(instance_) == class_) return instance_;
    DropInstance();
    Object* outer = GameInstance();
    Object* initialize = Function(L"Initialize");
    if (!outer || !initialize) return nullptr;
    Call spawn(ue::Default("/Script/Engine.Default__GameplayStatics"), "SpawnObject");
    Object* created = spawn.Set("ObjectClass", class_).Set("Outer", outer).Invoke() ? spawn.Get<Object*>("ReturnValue") : nullptr;
    if (!ue::Alive(created)) return nullptr;
    if (!ue::AddReference(created)) { Log("SUPPLEMENT instance not kept: engine reference code differs from the pinned build"); return nullptr; }
    instance_ = created;
    const bool world = GiveWorld(instance_);
    if (!world) Log("SUPPLEMENT world context unavailable: the instance's GetWorld differs from the pinned build");
    Call(instance_, initialize).Invoke();
    // The events' HelpObj parameter is the menu entry the native menu passed: the event-NPC items teleport the player
    // and add quest tags through it (TeleportCharacter / AddEventTags), so they get a live one of the native class.
    if (Object* help_class = ue::FindObject(kHelpObjClass)) {
        Call help(ue::Default("/Script/Engine.Default__GameplayStatics"), "SpawnObject");
        Object* made = help.Set("ObjectClass", help_class).Set("Outer", instance_).Invoke() ? help.Get<Object*>("ReturnValue") : nullptr;
        if (ue::Alive(made) && ue::AddReference(made)) { help_ = made; GiveWorld(help_); }
        RestoreHelpExecs();
    }
    Logf("SUPPLEMENT instance=%p outer=%p help=%p world=%d initialized", static_cast<void*>(instance_), static_cast<void*>(outer),
        static_cast<void*>(help_), world ? 1 : 0);
    return instance_;
}

void Refuse(int32_t index, const wchar_t* status) { Publish(status, index, false); }
// What the item code reaches: the GetWorld in the instance's vtable, the player as the Blueprint resolves it with the
// instance as world context versus the live world, and the debug invulnerability tag count on the player's ability system.
void Probe() {
    Object* instance = Instance();
    if (!instance) { Log("SUPPLEMENT_PROBE no instance"); return; }
    auto slot = [](Object* o) { return o ? reinterpret_cast<uintptr_t>((*reinterpret_cast<void***>(o))[kGetWorldSlot]) - ue::Base() : 0; };
    Object* world = CurrentWorld();
    Object* by_instance = PlayerCharacter(instance), *by_world = PlayerCharacter(world);
    int32_t count = -1;
    if (by_world) {
        Call get(ue::Default("/Script/GameplayAbilities.Default__AbilitySystemBlueprintLibrary"), "GetAbilitySystemComponent");
        Object* asc = get.Set("Actor", by_world).Invoke() ? get.Get<Object*>("ReturnValue") : nullptr;
        struct Tag { Name name; } tag{ue::MakeName(L"CharacterState.Stand.Invulnerable.Debug")};
        Call tags(asc, "GetGameplayTagCount");
        if (asc && tags.Set("GameplayTag", tag).Invoke()) count = tags.Get<int32_t>("ReturnValue");
    }
    Vector at{};
    if (by_world) { Call where(by_world, "K2_GetActorLocation"); if (where.Invoke()) at = where.Get<Vector>("ReturnValue"); }
    Object* movement = PlayerMovement();
    const int32_t mode_offset = movement ? ue::Offset(ue::ClassOf(movement), "MovementMode") : -1;
    const int mode = mode_offset >= 0 ? ue::At<uint8_t>(movement, mode_offset) : -1;
    Logf("SUPPLEMENT_PROBE instance=%p GetWorld=%#llx help GetWorld=%#llx outer GetWorld=%#llx (UObject %#llx) world=%p "
        "player by instance=%p by world=%p invulnerable.debug=%d at=%.0f %.0f %.0f mode=%d fly_guard=%d", static_cast<void*>(instance),
        static_cast<unsigned long long>(slot(instance)), static_cast<unsigned long long>(slot(help_)),
        static_cast<unsigned long long>(slot(ue::OuterOf(instance))), static_cast<unsigned long long>(kObjectGetWorldRva),
        static_cast<void*>(world), static_cast<void*>(by_instance), static_cast<void*>(by_world), count, at.x, at.y, at.z, mode, fly_guard_ ? 1 : 0);
    if (help_) Call(help_, "AddEventTags").Invoke();  // an empty container: our exec and the manager lookup, no tags added
}
// Diagnostics for restoring the emptied debug natives: parameters of the functions the 10-29 code called, the classes
// that own them, and the row layout of the data tables it read.
void DumpProperties(const char* what, Object* strukt, int limit) {
    int n = 0;
    for (Object* s = strukt; s && n < limit; s = ue::At<Object*>(s, ue::layout::kSuper))
        for (void* p = ue::At<void*>(s, ue::layout::kChildProperties); p && n < limit; p = ue::At<void*>(p, ue::layout::kPropNext), ++n) {
            char name[96]{}, type[64]{};
            ue::NameText(ue::At<Name>(p, ue::layout::kPropName), name, sizeof(name));
            if (void* field_class = ue::At<void*>(p, 0x8)) ue::NameText(ue::At<Name>(field_class, 0), type, sizeof(type));
            Logf("SUPPLEMENT_REFLECT %s %s %s off=%#x size=%d", what, type, name, ue::At<int32_t>(p, ue::layout::kPropOffset),
                ue::At<int32_t>(p, ue::layout::kPropElementSize));
        }
}
Object* OwnerOfFunction(const char* function) {
    Object* function_class = ue::FindObject("/Script/CoreUObject.Function");
    Object* found = nullptr;
    ue::ForEachObject([&](Object* o) {
        if (ue::NameIs(ue::NameOf(o), function) && ue::ClassOf(o) == function_class) { found = ue::OuterOf(o); return false; }
        return true;
    });
    return found;
}
void DumpFunction(const char* function) {
    Object* owner = OwnerOfFunction(function);
    char cls[128]{};
    if (owner) ue::NameText(ue::NameOf(owner), cls, sizeof(cls));
    Logf("SUPPLEMENT_REFLECT function %s owner=%s", function, owner ? cls : "-");
    if (owner) DumpProperties(function, ue::FindFunction(owner, function), 16);
}
void DumpTable(const wchar_t* package, const wchar_t* asset) {
    Object* table = LoadAsset(package, asset);
    const int32_t row_struct = table ? ue::Offset(ue::ClassOf(table), "RowStruct") : -1;
    Object* strukt = row_struct >= 0 ? ue::At<Object*>(table, row_struct) : nullptr;
    char name[96]{};
    if (strukt) ue::NameText(ue::NameOf(strukt), name, sizeof(name));
    const ue::Array rows = row_struct >= 0 ? ue::At<ue::Array>(table, row_struct + 8) : ue::Array{};
    Logf("SUPPLEMENT_REFLECT table %s=%p struct=%s rowmap data=%p num=%d max=%d", Utf8(asset).c_str(), static_cast<void*>(table), name,
        rows.data, rows.num, rows.max);
    if (!strukt) return;
    DumpProperties(name, strukt, 60);
    for (int32_t i = 0; i < rows.num && i < 4; ++i) {
        unsigned char* element = static_cast<unsigned char*>(rows.data) + i * 24;
        char key[96]{}, first[96]{};
        ue::NameText(ue::At<Name>(element, 0), key, sizeof(key));
        unsigned char* row = ue::At<unsigned char*>(element, 8);
        if (row) ue::NameText(ue::At<Name>(row, 8), first, sizeof(first));
        Logf("SUPPLEMENT_REFLECT row %d key=%s row=%p first=%s b26c=%d tags630=%d", i, key, static_cast<void*>(row), first,
            row ? row[0x26c] : -1, row ? ue::At<int32_t>(row, 0x638) : -1);
    }
}
void Reflect() {
    for (const char* f : {"PushAccessoryToInventory", "PushMedicineToInventory", "AddPropsToBackPack", "AddPropsSilently", "GetAllItems",
             "AddTags", "GetAllAttributes", "GetFloatAttributeFromAbilitySystemComponent", "GetDebugStringFromGameplayAttribute",
             "ExecuteTraceLogic", "ExecuteAutomatedCameraLogic", "GM_GetAllItems", "GetAllAttributeValue", "GetRuntimeGridDebugName",
             "GetEngineSubsystem", "GetGameInstanceSubsystem"})
        DumpFunction(f);
    DumpTable(L"/Game/PBZContent/DataTable/UI/DT_Accessory", L"DT_Accessory");
    DumpTable(L"/Game/PBZContent/DataTable/UI/DT_MedicineTable", L"DT_MedicineTable");
    DumpTable(L"/Game/PBZContent/DataTable/Item/DT_InventoryData", L"DT_InventoryData");
    DumpTable(L"/Game/PBZContent/DataTable/UI/DT_WeaponData", L"DT_WeaponData");
    DumpTable(L"/Game/PBZContent/DataTable/UI/DT_ShaqiWeaponData", L"DT_ShaqiWeaponData");
    Log("SUPPLEMENT_REFLECT done");
}
// The help object's TeleportCharacter through ProcessEvent (the exec's explicit-property path); the item code uses the
// script path.
void TeleportThroughHelp(const Request& request) {
    if (!Instance() || !help_) { Log("SUPPLEMENT teleport refused: no help object"); return; }
    const Vector location{request.x, request.y, request.z};
    Object* function = ue::FindFunction(ue::ClassOf(help_), "TeleportCharacter");
    char name[64]{};
    if (!FirstParameter(function, sizeof(Vector), name, sizeof(name)) || !Call(help_, function).Set(name, location).Invoke())
        Log("SUPPLEMENT teleport refused: TeleportCharacter(vector) not found");
}
bool SequencePlaying(bool& playing) {
    Object* manager = Subsystem("/Script/ProjectPBZ.PBZLevelSequenceManager");
    Call query(manager, "IsInSequence");
    if (!manager || !query.Slot("ReturnValue", sizeof(bool)) || !query.Invoke()) return false;
    playing = query.Get<bool>("ReturnValue");
    return true;
}
// Keys come from the cooked catalog, including its actor/node indices. Query
// the row through the engine; do not gate playback on a guessed FArrayProperty
// or sparse RowMap layout (the previous preflight rejected valid cooked rows).
bool SequenceExists(const std::wstring& text) {
    SupplementSequence::Key key;
    if(!SupplementSequence::Parse(text,key)){Log("SUPPLEMENT sequence preflight: malformed key");return false;}
    bool authored=false;
    for(const auto& item:DebugSupplementCatalog::kItems)
        if(std::wcscmp(item.category,L"播放Seq")==0&&*item.arg&&text==item.arg){authored=true;break;}
    if(!authored){Logf("SUPPLEMENT sequence preflight: key not in cooked catalog %s",Utf8(text).c_str());return false;}
    Object* table=LoadAsset(L"/Game/PBZContent/Blueprint/FlowGraph/DebugUI/FlowLogic/FlowLogicPlaySeqTable",L"FlowLogicPlaySeqTable");
    Call exists(ue::Default("/Script/Engine.Default__DataTableFunctionLibrary"),"DoesDataTableRowExist");
    if(!table||!exists.Slot("ReturnValue",sizeof(bool))||
       !exists.Set("Table",table).Set("RowName",ue::MakeName(key.row.c_str())).Invoke()){
        Logf("SUPPLEMENT sequence preflight: table=%p native row query unavailable",static_cast<void*>(table));return false;}
    const bool found=exists.Get<bool>("ReturnValue");
    Logf("SUPPLEMENT sequence preflight: native row=%s found=%d actor=%d node=%d",Utf8(key.row).c_str(),found?1:0,key.actor,key.node);
    return found;
}

void FinishSequence(bool ok, const wchar_t* reason) {
    const int item = sequence_request_.item;
    if (item < 0) return;
    const auto& entry = DebugSupplementCatalog::kItems[item];
    const std::wstring status = std::wstring(reason) + L"：" + entry.label;
    Logf("SUPPLEMENT sequence #%d ok=%d %s", item, ok ? 1 : 0, Utf8(status).c_str());
    sequence_request_.Clear();
    if (ok) ++calls_;
    Publish(status.c_str(), item, ok);
}
void DriveSequence() {
    if (sequence_request_.item < 0) return;
    Object* world = CurrentWorld();
    bool playing = false;
    const bool ready = world && SequencePlaying(playing);
    Call pause(ue::Default("/Script/Engine.Default__GameplayStatics"), "IsGamePaused");
    const bool paused = !world || !pause.Slot("ReturnValue", sizeof(bool)) ||
        !pause.Set("WorldContextObject", world).Invoke() || pause.Get<bool>("ReturnValue");
    using Step = SupplementSequence::Request::Step;
    switch (sequence_request_.Poll(GetTickCount64(), ready, paused, playing)) {
    case Step::Started: FinishSequence(true, L"已开始播放"); return;
    case Step::Timeout: FinishSequence(false, L"播放超时，未检测到过场开始"); return;
    case Step::Wait: return;
    case Step::Invoke: break;
    }
    if (playing) { FinishSequence(false, L"当前过场尚未结束，请结束后再播放"); return; }
    const auto& item = DebugSupplementCatalog::kItems[sequence_request_.item];
    Object* instance = Instance();
    Object* event = Function(item.event);
    const int key_offset = instance ? ue::Offset(class_, "FlowPlaySeqID") : -1;
    std::wstring key = item.arg;
    if (key.empty() && key_offset >= 0) key = WideName(ue::At<Name>(instance, key_offset));
    if (!instance || !event || key_offset < 0) {
        Logf("SUPPLEMENT sequence preflight: instance=%p event=%p keyOffset=%d",static_cast<void*>(instance),static_cast<void*>(event),key_offset);
        FinishSequence(false,L"无法播放，过场控制器未就绪");return;
    }
    if(!SequenceExists(key)){FinishSequence(false,L"无法播放，过场表项校验失败（详细原因已记录）");return;}
    Call call(instance, event);
    if (*item.arg) {
        // Fail closed: never invoke a zero/None SeqID when the signature differs.
        if (!call.Slot("SeqID", sizeof(Name))) { FinishSequence(false, L"无法播放，过场参数不兼容"); return; }
        call.Set("SeqID", ue::MakeName(key.c_str()));
    }
    const Name previous_key=ue::At<Name>(instance,key_offset);
    if(*item.arg)ue::At<Name>(instance,key_offset)=Name{};
    active_item_ = sequence_request_.item;
    const bool ran = call.Invoke();
    active_item_ = -1;
    if (!ran || ue::At<Name>(instance, key_offset).None()) {
        ue::At<Name>(instance,key_offset)=previous_key;
        FinishSequence(false, L"播放请求未被过场流程接受"); return;
    }
    Logf("SUPPLEMENT sequence #%d dispatched key=%s; awaiting IsInSequence", sequence_request_.item, Utf8(key).c_str());
    Publish((std::wstring(L"正在准备过场：")+item.label).c_str());
}
void Tick() {
    if (!mount_tried_) {
        Mount();
        Publish(mounted_ ? L"补充包已挂载" : L"补充包挂载失败");
    }
    GuardFlight();
    DriveSequence();
    Request request;
    AcquireSRWLockExclusive(&queue_lock);
    const bool any = !queue_.empty();
    if (any) { request = queue_.front(); queue_.pop_front(); }
    ReleaseSRWLockExclusive(&queue_lock);
    if (!any) return;
    const int32_t finished = request.action == Action::Call ? (std::max)(request.item, 0) : -1;
    if (!mounted_) { Refuse(finished, L"补充包挂载失败，不能加载其中的蓝图"); return; }
    if (!Prepare()) { Refuse(finished, L"补充包已挂载，但其中的蓝图没有加载"); return; }
    int32_t available = 0;
    for (size_t i = 0; i < kItemCount; ++i) available += available_[i] && !DebugSupplementCatalog::kItems[i].removed ? 1 : 0;
    wchar_t status[200]{};
    if (request.action == Action::Probe) { Probe(); return; }
    if (request.action == Action::Teleport) { TeleportThroughHelp(request); return; }
    if (request.action == Action::Reflect) { Reflect(); return; }
    if (request.action == Action::Load) {
        _snwprintf_s(status, _countof(status), _TRUNCATE, L"已加载：%d / %zu 项可用", available, kItemCount);
        Publish(status);
        return;
    }
    if (request.item < 0 || request.item >= static_cast<int32_t>(kItemCount)) { Refuse(finished, L"没有这一项"); return; }
    const DebugSupplementCatalog::Item& item = DebugSupplementCatalog::kItems[request.item];
    if (item.removed || !available_[request.item]) {
        Logf("SUPPLEMENT call refused #%d %s: %s", request.item, Utf8(item.label).c_str(), item.removed ? "removed by the game" : "missing functions");
        _snwprintf_s(status, _countof(status), _TRUNCATE, item.removed ? L"当前游戏版本不支持这一项：%ls" : L"当前游戏缺少这一项需要的函数：%ls", item.label);
        Refuse(request.item, status);
        return;
    }
    if (!std::wcscmp(item.category, L"播放Seq")) {
        if (!sequence_request_.Begin(request.item, GetTickCount64())) {
            Refuse(request.item, L"已有过场正在准备，请等待播放结果"); return;
        }
        Publish((std::wstring(L"正在准备过场：")+item.label).c_str());
        return;
    }
    if (*item.native) {
        active_item_ = request.item;
        const bool ran = RunCheat(item.native);
        active_item_ = -1;
        if (ran) ++calls_;
        Logf("SUPPLEMENT call #%d %s / %s / %s cheat=%s ok=%d", request.item, Utf8(item.category).c_str(), Utf8(item.group).c_str(),
            Utf8(item.label).c_str(), item.native, ran ? 1 : 0);
        _snwprintf_s(status, _countof(status), _TRUNCATE, ran ? L"已执行：%ls" : L"执行失败：%ls", item.label);
        Publish(status, request.item, ran);
        return;
    }
    Object* instance = Instance();
    Object* event = Function(item.event);
    if (!instance || !event) {
        Logf("SUPPLEMENT call refused #%d %s: no game instance or spawn failed", request.item, Utf8(item.label).c_str());
        Refuse(request.item, L"不能执行：还没有进入游戏");
        return;
    }
    active_item_ = request.item;
    Call call(instance, event);
    if (unsigned char* slot = call.Slot("HelpObj", sizeof(Object*))) std::memcpy(slot, &help_, sizeof(help_));
    char argument[64]{};
    if (*item.arg) {  // 播放Seq: TryPlaySeqById(SeqID), the play key the menu entry carried
        if (FirstParameter(event, sizeof(Name), argument, sizeof(argument))) call.Set(argument, ue::MakeName(item.arg));
        else Logf("SUPPLEMENT call #%d: %s has no name parameter", request.item, Utf8(item.event).c_str());
    }
    const bool ok = call.Invoke();
    active_item_ = -1;
    if (ok) ++calls_;
    Logf("SUPPLEMENT call #%d %s / %s / %s event=%s ok=%d", request.item, Utf8(item.category).c_str(), Utf8(item.group).c_str(),
        Utf8(item.label).c_str(), Utf8(item.event).c_str(), ok ? 1 : 0);
    _snwprintf_s(status, _countof(status), _TRUNCATE, ok ? L"已执行：%ls" : L"执行失败：%ls", item.label);
    Publish(status, request.item, ok);
}
// The instance and its help object keep our references (their state is unknown after an exception).
void Fault() {
    RaiseBudgets(false);
    class_ = nullptr; instance_ = nullptr; help_ = nullptr; functions_.clear();
    Log("SUPPLEMENT request fault; state cleared");
    Publish(L"执行时出现异常，已重置", active_item_ >= 0 ? active_item_ : sequence_request_.item, false);
    sequence_request_.Clear();
    active_item_ = -1;
}
bool Enqueue(Request request) {
    if (!present_) return false;
    AcquireSRWLockExclusive(&queue_lock);
    const bool ok = queue_.size() < 32;
    if (ok) queue_.push_back(request);
    ReleaseSRWLockExclusive(&queue_lock);
    return ok;
}
}

void Initialize(Logger logger, const wchar_t* executable_directory) {
    logger_ = logger;
    wchar_t paks[MAX_PATH * 2]{};
    const std::wstring relative = std::wstring(executable_directory) + L"..\\..\\ProjectPBZ\\Content\\Paks\\";
    if (!GetFullPathNameW(relative.c_str(), _countof(paks), paks, nullptr)) wcsncpy_s(paks, relative.c_str(), _TRUNCATE);
    const std::wstring found = FindContainer(paks, 3);
    present_ = !found.empty();
    if (present_) pak_path_ = found + kContainer + L".pak";
    Logf("SUPPLEMENT %s %s", present_ ? "container found in" : "container absent under", Utf8(present_ ? found : std::wstring(paks)).c_str());
    AcquireSRWLockExclusive(&state_lock);
    for (const auto& row : DebugSupplementCatalog::kItems) state_.items.push_back({row.category, row.group, row.label, row.removed, false});
    ReleaseSRWLockExclusive(&state_lock);
    Publish(present_ ? L"已找到补充包" : L"未找到补充包");
}
bool Present() { return present_; }
bool NeedsWorld() { return sequence_request_.item >= 0; }
Snapshot GetSnapshot() {
    AcquireSRWLockShared(&state_lock);
    Snapshot copy = state_;
    ReleaseSRWLockShared(&state_lock);
    return copy;
}
bool RequestLoad() { return Enqueue({Action::Load, -1}); }
bool RequestCall(int32_t item) { return Enqueue({Action::Call, item}); }
bool HandleCommand(const char* verb, const char* argument) {
    if (std::strcmp(verb, "supplement")) return false;
    if (!*argument || !std::strcmp(argument, "status") || !std::strcmp(argument, "list")) {
        const Snapshot s = GetSnapshot();
        Logf("SUPPLEMENT status present=%d mounted=%d loaded=%d items=%zu calls=%u: %s", s.present ? 1 : 0, s.mounted ? 1 : 0, s.loaded ? 1 : 0,
            s.items.size(), s.calls, Utf8(s.status).c_str());
        for (size_t i = 0; !std::strcmp(argument, "list") && i < s.items.size(); ++i)
            Logf("SUPPLEMENT_ITEM #%zu %s / %s / %s available=%d removed=%d", i, Utf8(s.items[i].category).c_str(), Utf8(s.items[i].group).c_str(),
                Utf8(s.items[i].label).c_str(), s.items[i].available ? 1 : 0, s.items[i].removed ? 1 : 0);
        return true;
    }
    if (!std::strcmp(argument, "load")) return RequestLoad();
    if (!std::strcmp(argument, "probe")) return Enqueue({Action::Probe, -1});
    if (!std::strcmp(argument, "reflect")) return Enqueue({Action::Reflect, -1});
    Request teleport{Action::Teleport, -1};
    if (sscanf_s(argument, "teleport %lf %lf %lf", &teleport.x, &teleport.y, &teleport.z) == 3) return Enqueue(teleport);
    if (!std::strncmp(argument, "call ", 5)) {
        char* end = nullptr;
        const long item = std::strtol(argument + 5, &end, 10);
        return end != argument + 5 && !*end && RequestCall(static_cast<int32_t>(item));
    }
    return false;
}
void OnGameThreadGuarded() {
    if (!present_) return;
    __try { Tick(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { Fault(); }
}
}
