#include "groom_force.h"
#include "game_thread.h"
#include "ue_runtime.h"
#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <iterator>
#include <string>
#include <vector>

namespace GroomForce {
namespace {
using ue::Array; using ue::Call; using ue::Name; using ue::Object;

// Every blueprint with serialized GroomComponent templates (usage/groom_component_refs.json).
constexpr const char* kDonors[] = {
    "/HuaShira/Animation/Seqs/ActorForSeqs/BP_Seq_Hun", "/HuaShira/Animation/Seqs/ActorForSeqs/BP_Seq_Hun_Climax",
    "/HuaShira/Animation/Seqs/ActorForSeqs/BP_Seq_Young_Hun", "/HuaShira/Animation/Seqs/ActorForSeqs/BP_Seq_ZuoShang",
    "/HuaShira/Animation/Seqs/ActorForSeqs/BP_Seq_ZuoShang_White", "/HuaShira/Animation/Seqs/ActorForSeqs/BP_Seq_ZuoShang_LongTi",
    "/HuaShira/Animation/Seqs/ActorForSeqs/BP_Seq_Young_ZuoShang", "/HuaShira/Animation/Seqs/ActorForSeqs/BP_Seq_MoTian",
    "/HuaShira/Animation/Seqs/ActorForSeqs/BP_Seq_MoTian_Old", "/HuaShira/Animation/Seqs/ActorForSeqs/BP_Seq_MoTian_Old_Mask",
    "/HuaShira/Animation/Seqs/ActorForSeqs/BP_Seq_ChangHong", "/HuaShira/Animation/Seqs/ActorForSeqs/BP_Seq_ChangHong_P2",
    "/HuaShira/Animation/Seqs/ActorForSeqs/BP_Seq_ChiLian_P2", "/HuaShira/Animation/Seqs/ActorForSeqs/BP_Seq_GuWanYue",
    "/HuaShira/Animation/Seqs/ActorForSeqs/BP_Seq_YanQianSui", "/PBZCombatAsset/ActorData/Mobs/073_YanQianSui/BP_Monster_YanQianSui",
    "/HuaShira/Animation/Seqs/ActorForSeqs/BP_Seq_MuXiaoKui", "/HuaShira/Animation/Seqs/ActorForSeqs/BP_Seq_MuXiaoKui_P2",
    "/Game/PBZContent/Blueprint/Character/NPC/FlowNpc/BP_NPC_MuXiaoKui", "/Game/PBZContent/Blueprint/Character/NPC/FlowNpc/BP_NPC_MuXiaoKui_P2",
    "/HuaShira/Animation/Seqs/ActorForSeqs/BP_Seq_YuEr", "/HuaShira/Animation/Seqs/ActorForSeqs/BP_Seq_HuanHunYuEr",
    "/Game/PBZContent/Blueprint/Character/NPC/FlowNpc/BP_NPC_YuEr", "/Game/PBZContent/Blueprint/Character/NPC/FlowNpc/BP_NPC_HuanHunYuEr",
    "/Game/PBZContent/Blueprint/Character/NPC/FlowNpc/BP_NPC_Ending_YuEr", "/HuaShira/Animation/Seqs/ActorForSeqs/BP_Seq_XuKong",
    "/HuaShira/Animation/Seqs/ActorForSeqs/BP_Seq_XuKong_HuanXiang", "/Game/PBZContent/Blueprint/Character/NPC/FlowNpc/BP_NPC_XuKong",
    "/Game/PBZContent/Blueprint/Character/NPC/FlowNpc/BP_NPC_Xukong_HuanXiang", "/HuaShira/Animation/Seqs/ActorForSeqs/BP_Seq_XianZhe",
    "/Game/PBZContent/Blueprint/Character/NPC/FlowNpc/BP_NPC_XianZhe", "/HuaShira/Animation/Seqs/ActorForSeqs/BP_Seq_HuangXingZi",
    "/HuaShira/Animation/Seqs/ActorForSeqs/BP_Seq_XiaoZhang", "/HuaShira/Animation/Seqs/ActorForSeqs/BP_Seq_MuTianMiao",
    "/HuaShira/Animation/Seqs/ActorForSeqs/BP_Seq_LianXie", "/HuaShira/Animation/Seqs/ActorForSeqs/BP_Seq_HuanGui",
    "/HuaShira/Animation/Seqs/ActorForSeqs/BP_Seq_ZhenYao", "/HuaShira/Animation/Seqs/ActorForSeqs/BP_Seq_ZhongShu",
    "/HuaShira/Animation/Seqs/ActorForSeqs/BP_Seq_XiNiang", "/HuaShira/Animation/Seqs/ActorForSeqs/BP_Seq_LanShaoDi",
};
constexpr const char* kHairWords[] = {"hair", "brow", "lash", "beard", "bread", "mustache", "bangs"};
constexpr const char* kKeepWords[] = {"band", "hat", "head"};
constexpr const char* kPrefixes[] = {"bp_seq_", "bp_npc_", "bp_monster_"};
constexpr size_t kGroomDataStride = 0xf8;        // sizeof(FAppearanceGroomData), RefreshGroomComponents imul
constexpr size_t kLocalBoneString = 0x08 + 0x10; // SimulationSettings.SimulationSetup.LocalBone (FString)
constexpr ULONGLONG kScanInterval = 3000;

struct alignas(16) Transform { double rotation[4]{0, 0, 0, 1}, translation[4]{}, scale[4]{1, 1, 1, 0}; };
static_assert(sizeof(Transform) == 96);
struct SoftPath { int32_t index = 0, serial = 0; Name package, asset; ue::String sub; };
static_assert(sizeof(SoftPath) == 40);

struct MeshFlag { Object* component; uint8_t original; Name name; };
// Actors are tracked by pointer and FName: after a map change a new actor can be built at a
// freed one's address, but spawned actors get a new name number.
struct Seen { Object* actor; Name name; };
struct Forced {
    Object* actor = nullptr;
    Name name;
    Object* appearance = nullptr;  // player: groom set owned by the native appearance component
    Name appearance_name;
    std::vector<Object*> parents;
    Name player_parent_name;
    Object* player_parent_mesh = nullptr;
    std::vector<Object*> grooms;   // native player grooms, or components added here
    std::vector<Name> groom_names;
    std::vector<MeshFlag> meshes;
    std::string label;
    bool had_grooms = false;       // native Grooms were seen at least once
    bool retiring_for_external = false;
};
struct Donor { const char* path; Object* cls = nullptr; bool tried = false; std::vector<Object*> templates; };

Logger log_ = nullptr;
ExternalAppearanceOwned external_owned_ = nullptr;
bool ExternalOwnsPlayer() { return external_owned_ && external_owned_(); }
Object* engine_ = nullptr;
volatile LONG request_ = kNone, ready_ = 0, active_ = 0;
SRWLOCK status_lock = SRWLOCK_INIT;
wchar_t status_[320] = L"Force Groom: waiting for the engine tick";
bool validated_ = false, broken_ = false;
ULONGLONG next_validate_ = 0;
std::vector<Forced> forced_;
std::vector<Seen> skipped_;
std::vector<Donor> donors_;
ULONGLONG next_scan_ = 0;
Object* world_ = nullptr;
Name world_name_;
Object* current_ = nullptr;           // actor being forced or released, for fault reports
const char* stage_ = "idle";
volatile LONG outfit_request_ = -1, outfit_active_ = 0;
wchar_t outfit_status_[200] = L"Outfit unlock OFF (Shift+F6)";
Object* outfit_component_ = nullptr;
ULONGLONG next_outfit_ = 0;
bool outfit_disable_deferred_ = false;

void Logf(const char* format, ...) {
    if (!log_) return;
    char line[600]{}; va_list args; va_start(args, format); vsprintf_s(line, format, args); va_end(args); log_(line);
}
void SetStatus(const wchar_t* format, ...) {
    wchar_t text[320]{}; va_list args; va_start(args, format); vswprintf_s(text, format, args); va_end(args);
    AcquireSRWLockExclusive(&status_lock); wcscpy_s(status_, text); ReleaseSRWLockExclusive(&status_lock);
}
std::string Text(Object* o) { char t[160]{}; if (o) ue::NameText(ue::NameOf(o), t, sizeof(t)); return t; }
bool Same(Object* actor, Name name) { return ue::Alive(actor) && ue::NameOf(actor) == name; }
std::string Lower(std::string s) { for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c))); return s; }

Object* Cls(const char* path) { return ue::FindObject(path); }
int32_t Off(const char* strukt, const char* property) { return ue::Offset(Cls(strukt), property); }
Object* Kismet() { return ue::Default("/Script/Engine.Default__KismetSystemLibrary"); }
Object* GroomClass() { return Cls("/Script/HairStrandsCore.GroomComponent"); }

std::vector<Object*> Components(Object* actor, Object* cls) {
    std::vector<Object*> out;
    Call c(actor, "K2_GetComponentsByClass");
    if (!c.Set("ComponentClass", cls).Invoke()) return out;
    const Array a = c.Get<Array>("ReturnValue");
    for (int32_t i = 0; i < a.num; ++i) if (Object* o = static_cast<Object**>(a.data)[i]) out.push_back(o);
    ue::Free(a.data);
    return out;
}
Object* MeshOf(Object* component) {
    Object* asset = ue::At<Object*>(component, Off("/Script/Engine.SkinnedMeshComponent", "SkinnedAsset"));
    return asset ? asset : ue::At<Object*>(component, Off("/Script/Engine.SkinnedMeshComponent", "SkeletalMesh"));
}
Object* ParentFor(Object* binding, const std::vector<Object*>& skinned) {
    if (!binding) return nullptr;
    Object* target = ue::At<Object*>(binding, Off("/Script/HairStrandsCore.GroomBindingAsset", "TargetSkeletalMesh"));
    for (Object* c : skinned) if (target && MeshOf(c) == target) return c;
    return nullptr;
}
void ApplySkinned(Object* c) { reinterpret_cast<void(__fastcall*)(Object*)>(ue::Base() + ue::layout::kApplySkinnedHairMode)(c); }
void ApplyGroom(Object* c) { reinterpret_cast<void(__fastcall*)(Object*)>(ue::Base() + ue::layout::kApplyGroomHairMode)(c); }

// Mesh hair of the actor joins MainHairMode so the game hides it while Grooms show.
void FlagMeshHair(Forced& f, const std::vector<Object*>& skinned) {
    const int32_t flag = Off("/Script/Engine.SkinnedMeshComponent", "bInfluencedByMainHairMode");
    for (Object* c : skinned) {
        bool parent = false;
        for (Object* p : f.parents) parent |= p == c;
        Object* mesh = MeshOf(c);
        const bool hair = ue::NameContains(ue::NameOf(c), kHairWords, std::size(kHairWords)) ||
            (mesh && ue::NameContains(ue::NameOf(mesh), kHairWords, std::size(kHairWords)));
        const bool keep = ue::NameContains(ue::NameOf(c), kKeepWords, std::size(kKeepWords)) ||
            (mesh && ue::NameContains(ue::NameOf(mesh), kKeepWords, std::size(kKeepWords)));
        if (parent || !hair || keep) continue;
        uint8_t& bits = ue::At<uint8_t>(c, flag);
        f.meshes.push_back({c, bits, ue::NameOf(c)});
        bits |= 2;
        ApplySkinned(c);
    }
}

// The appearance component may create or finish its Grooms after async loads; flag
// every Groom under the parent (again on later scans) so MainHairMode governs it.
void CollectNative(Forced& f) {
    if (f.parents.empty() || !ue::Alive(f.actor)) return;
    const int32_t attach = Off("/Script/Engine.SceneComponent", "AttachParent"), main = Off("/Script/HairStrandsCore.GroomComponent", "bIsMainHair");
    f.grooms.clear(); f.groom_names.clear();
    for (Object* g : Components(f.actor, GroomClass())) {
        if (ue::At<Object*>(g, attach) != f.parents.front()) continue;
        if (!ue::At<uint8_t>(g, main)) { ue::At<uint8_t>(g, main) = 1; ApplyGroom(g); }
        f.grooms.push_back(g); f.groom_names.push_back(ue::NameOf(g));
    }
    f.had_grooms |= !f.grooms.empty();
}

// Player: current outfit -> its *_Seq hairstyle row -> native RefreshGroomComponents.
bool ForcePlayer(Forced& f, const std::vector<Object*>& skinned) {
    const std::vector<Object*> apps = Components(f.actor, Cls("/Script/ProjectPBZ.PBZAppearanceComponent"));
    if (apps.empty()) return false;
    Object* app = apps.front();
    Call effective(app, "GetEffectiveOutfit");
    Name outfit = effective.Invoke() ? effective.Get<Name>("ReturnValue") : Name{};
    for (const char* fallback : {"DefaultVisualOutfitID", "DefaultOutfitID"})
        if (outfit.None()) outfit = ue::At<Name>(app, Off("/Script/ProjectPBZ.PBZAppearanceComponent", fallback));
    Call outfit_row(app, "GetAppearanceStruct");
    unsigned char* outfit_struct = outfit_row.Slot("ReturnValue", 800);
    if (outfit.None() || !outfit_struct || !outfit_row.Set("AppearanceItemID", outfit).Invoke()) return false;
    const Name sequence = ue::At<Name>(outfit_struct, Off("/Script/ProjectPBZ.PBZAppearanceStruct", "OutfitSequenceHairStyleItem"));
    char outfit_text[96]{}, row_text[96]{};
    ue::NameText(outfit, outfit_text, sizeof(outfit_text)); ue::NameText(sequence, row_text, sizeof(row_text));
    if (sequence.None()) { Logf("FORCE_GROOM player outfit=%s has no sequence hairstyle row", outfit_text); return false; }
    // The returned struct copies are leaked on purpose (a few KiB per press): their
    // FText/TArray members belong to the engine allocator and nothing here owns a destructor.
    Call hair_row(app, "GetAppearanceStruct");
    unsigned char* hair_struct = hair_row.Slot("ReturnValue", 800);
    if (!hair_struct || !hair_row.Set("AppearanceItemID", sequence).Invoke()) return false;
    const Array data = ue::At<Array>(hair_struct, Off("/Script/ProjectPBZ.PBZAppearanceStruct", "GroomDataArray"));
    if (data.num <= 0 || data.num > 64 || !data.data) { Logf("FORCE_GROOM player row=%s has no GroomDataArray", row_text); return false; }
    const int32_t binding_offset = Off("/Script/ProjectPBZ.AppearanceGroomData", "GroomBindingAsset");
    Object* parent = nullptr;
    for (int32_t i = 0; i < data.num && !parent; ++i) {
        Call load(Kismet(), "LoadAsset_Blocking");
        unsigned char* soft = load.Slot("Asset", sizeof(SoftPath));
        if (!soft) break;
        std::memcpy(soft, static_cast<unsigned char*>(data.data) + i * kGroomDataStride + binding_offset, sizeof(SoftPath));
        if (load.Invoke()) parent = ParentFor(load.Get<Object*>("ReturnValue"), skinned);
    }
    if (!parent) { Logf("FORCE_GROOM player row=%s binding target mesh not on actor", row_text); return false; }
    Call refresh(app, "RefreshGroomComponents");
    if (!refresh.Set("ParentComponent", parent).Set("GroomDataArray", data).Invoke()) return false;
    f.appearance = app; f.appearance_name = ue::NameOf(app); f.parents.push_back(parent);
    f.player_parent_name = ue::NameOf(parent); f.player_parent_mesh = MeshOf(parent);
    f.label = std::string("player ") + row_text;
    CollectNative(f);
    Logf("FORCE_GROOM player outfit=%s row=%s entries=%d grooms=%zu parent=%s", outfit_text, row_text, data.num, f.grooms.size(), Text(parent).c_str());
    return true;
}

std::vector<std::string> Tokens(std::string name) {
    name = Lower(name);
    if (name.size() > 2 && name.compare(name.size() - 2, 2, "_c") == 0) name.resize(name.size() - 2);
    for (const char* p : kPrefixes) if (name.rfind(p, 0) == 0) { name.erase(0, std::strlen(p)); break; }
    std::vector<std::string> out; size_t start = 0;
    while (start <= name.size()) {
        const size_t cut = name.find('_', start);
        if (cut != start) out.push_back(name.substr(start, cut - start));
        if (cut == std::string::npos) break;
        start = cut + 1;
    }
    return out;
}
bool LoadDonor(Donor& d) {
    const std::string package(d.path), leaf = package.substr(package.find_last_of('/') + 1) + "_C";
    // Nothing here holds a reference to the donor class: the garbage collector frees it and its
    // Groom templates once no actor uses it (minutes later, or at a map change). Load it again then.
    if (d.cls && ue::FindObject((package + "." + leaf).c_str()) != d.cls) {
        Logf("FORCE_GROOM donor %s was unloaded; loading it again", d.path);
        d = Donor{d.path};
    }
    if (d.tried) return d.cls != nullptr;
    d.tried = true;
    d.cls = ue::FindObject((package + "." + leaf).c_str());
    if (!d.cls) {
        const std::wstring wpackage(package.begin(), package.end()), wleaf(leaf.begin(), leaf.end());
        SoftPath soft; soft.package = ue::MakeName(wpackage.c_str()); soft.asset = ue::MakeName(wleaf.c_str());
        Call load(Kismet(), "LoadClassAsset_Blocking");
        if (unsigned char* slot = load.Slot("AssetClass", sizeof(SoftPath))) {
            std::memcpy(slot, &soft, sizeof(soft));
            if (!soft.package.None() && !soft.asset.None() && load.Invoke()) d.cls = load.Get<Object*>("ReturnValue");
        }
    }
    if (!d.cls) { Logf("FORCE_GROOM donor %s failed to load", d.path); return false; }
    const int32_t asset = Off("/Script/HairStrandsCore.GroomComponent", "GroomAsset");
    std::vector<Object*> chain;
    for (Object* c = d.cls; c && !ue::NameIs(ue::NameOf(ue::ClassOf(c)), "Class"); c = ue::At<Object*>(c, ue::layout::kSuper)) chain.push_back(c);
    Object* groom = GroomClass();
    ue::ForEachObject([&](Object* o) {
        if (ue::ClassOf(o) != groom || !ue::At<Object*>(o, asset)) return true;
        for (Object* c : chain) if (ue::OuterOf(o) == c) { d.templates.push_back(o); break; }
        return true;
    });
    Logf("FORCE_GROOM donor %s loaded templates=%zu", d.path, d.templates.size());
    return !d.templates.empty();
}
std::vector<Donor*> MatchDonors(Object* actor) {
    if (donors_.empty()) for (const char* p : kDonors) donors_.push_back(Donor{p});
    const std::vector<std::string> target = Tokens(Text(ue::ClassOf(actor)));
    std::vector<std::pair<size_t, Donor*>> scored;
    for (Donor& d : donors_) {
        const std::string path(d.path);
        const std::vector<std::string> key = Tokens(path.substr(path.find_last_of('/') + 1));
        size_t hits = 0;
        for (const std::string& k : key) for (const std::string& t : target) if (k == t) { ++hits; break; }
        if (!key.empty() && hits == key.size()) scored.push_back({key.size(), &d});
    }
    std::stable_sort(scored.begin(), scored.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::vector<Donor*> out;
    for (auto& s : scored) out.push_back(s.second);
    return out;
}

// Other characters: copy the donor blueprint's Groom templates whose binding target mesh the actor has.
bool ForceDonor(Forced& f, const std::vector<Object*>& skinned) {
    const int32_t asset = Off("/Script/HairStrandsCore.GroomComponent", "GroomAsset"), binding = Off("/Script/HairStrandsCore.GroomComponent", "BindingAsset");
    const int32_t physics = Off("/Script/HairStrandsCore.GroomComponent", "PhysicsAsset"), main = Off("/Script/HairStrandsCore.GroomComponent", "bIsMainHair");
    const int32_t settings = Off("/Script/HairStrandsCore.GroomComponent", "SimulationSettings"), local = Off("/Script/HairStrandsCore.GroomComponent", "bForceLocalSpaceSimulation");
    const int32_t materials = Off("/Script/Engine.MeshComponent", "OverrideMaterials");
    for (Donor* d : MatchDonors(f.actor)) {
        if (!LoadDonor(*d)) continue;
        size_t unbound = 0;
        for (Object* t : d->templates) {
            Object* parent = ParentFor(ue::At<Object*>(t, binding), skinned);
            if (!parent) { ++unbound; continue; }
            Call add(f.actor, "AddComponentByClass");
            add.Set("Class", GroomClass()).Set("bManualAttachment", true).Set("RelativeTransform", Transform{}).Set("bDeferredFinish", true);
            Object* g = add.Invoke() ? add.Get<Object*>("ReturnValue") : nullptr;
            if (!g) continue;
            ue::At<Object*>(g, asset) = ue::At<Object*>(t, asset);
            ue::At<Object*>(g, binding) = ue::At<Object*>(t, binding);
            ue::At<Object*>(g, physics) = ue::At<Object*>(t, physics);
            ue::At<uint8_t>(g, main) = 1;
            ue::At<uint8_t>(g, local) = ue::At<uint8_t>(t, local);
            std::memcpy(static_cast<unsigned char*>(g) + settings, static_cast<unsigned char*>(t) + settings, 0x90);
            std::memset(static_cast<unsigned char*>(g) + settings + kLocalBoneString, 0, sizeof(ue::String));  // never share the FString buffer
            Call attach(g, "K2_AttachToComponent");
            attach.Set("Parent", parent).Set("LocationRule", uint8_t{2}).Set("RotationRule", uint8_t{2}).Set("ScaleRule", uint8_t{2}).Invoke();
            Call finish(f.actor, "FinishAddComponent");
            finish.Set("Component", g).Set("bManualAttachment", true).Set("RelativeTransform", Transform{}).Invoke();
            const Array m = ue::At<Array>(t, materials);
            for (int32_t i = 0; i < m.num && i < 64; ++i)
                if (Object* material = static_cast<Object**>(m.data)[i]) Call(g, "SetMaterial").Set("ElementIndex", i).Set("Material", material).Invoke();
            f.grooms.push_back(g); f.groom_names.push_back(ue::NameOf(g));
            bool known = false;
            for (Object* p : f.parents) known |= p == parent;
            if (!known) f.parents.push_back(parent);
        }
        Logf("FORCE_GROOM actor=%s donor=%s added=%zu unbound=%zu", Text(f.actor).c_str(), d->path, f.grooms.size(), unbound);
        if (!f.grooms.empty()) { f.label = Text(f.actor) + " <- " + Text(d->cls); return true; }
    }
    return false;
}

bool HasOwnGroom(Object* actor) {
    const int32_t asset = Off("/Script/HairStrandsCore.GroomComponent", "GroomAsset");
    for (Object* g : Components(actor, GroomClass())) if (ue::At<Object*>(g, asset)) return true;
    return false;
}
bool Known(Object* actor) {
    const Name name = ue::NameOf(actor);
    for (const Forced& f : forced_) if (f.actor == actor && f.name == name) return true;
    for (const Seen& s : skipped_) if (s.actor == actor && s.name == name) return true;
    return false;
}
void TryForce(Object* actor, bool player) {
    const std::string cls = Lower(Text(ue::ClassOf(actor)));
    const Name name = ue::NameOf(actor);
    if (ue::IsDefault(actor) || cls.rfind("bp_seq_", 0) == 0 || cls.find("preview") != std::string::npos || HasOwnGroom(actor)) {
        Logf("FORCE_GROOM skip %s (%s): cutscene/preview actor or already has Groom", Text(actor).c_str(), cls.c_str());
        skipped_.push_back({actor, name}); return;
    }
    current_ = actor;
    Forced f; f.actor = actor; f.name = name;
    const std::vector<Object*> skinned = Components(actor, Cls("/Script/Engine.SkinnedMeshComponent"));
    bool ok = player && ForcePlayer(f, skinned);
    if (!ok) { f = Forced{}; f.actor = actor; f.name = name; ok = ForceDonor(f, skinned); }
    current_ = nullptr;
    if (!ok) { Logf("FORCE_GROOM skip %s (%s): no matching Groom donor or binding mesh", Text(actor).c_str(), cls.c_str()); skipped_.push_back({actor, name}); return; }
    FlagMeshHair(f, skinned);
    Logf("FORCE_GROOM on %s grooms=%zu meshHairFlagged=%zu", f.label.c_str(), f.grooms.size(), f.meshes.size());
    forced_.push_back(std::move(f));
}
bool RestoreMeshFlags(Forced& f) {
    const int32_t flag = Off("/Script/Engine.SkinnedMeshComponent", "bInfluencedByMainHairMode");
    if (flag < 0) return f.meshes.empty();
    for (size_t i = f.meshes.size(); i-- > 0;) {
        const MeshFlag& m = f.meshes[i];
        if (Same(m.component, m.name) && !(m.original & 2)) {
            uint8_t& bits = ue::At<uint8_t>(m.component, flag);
            if (bits & 2) { bits &= static_cast<uint8_t>(~2u); ApplySkinned(m.component); }
            if (bits & 2) continue;
        }
        f.meshes.erase(f.meshes.begin() + i);
    }
    return f.meshes.empty();
}
bool ReleaseRecordForExternal(Forced& f);
bool Release(Forced& f) {
    if (f.retiring_for_external) return ReleaseRecordForExternal(f);
    current_ = ue::Alive(f.actor) ? f.actor : nullptr;
    if (f.appearance && ue::Alive(f.appearance) && !f.parents.empty() && ue::Alive(f.parents.front())) {
        Call refresh(f.appearance, "RefreshGroomComponents");
        refresh.Set("ParentComponent", f.parents.front()).Set("GroomDataArray", Array{}).Invoke();
    } else if (!f.appearance) {
        for (Object* g : f.grooms) if (ue::Alive(g)) Call(g, "K2_DestroyComponent").Set("Object", g).Invoke();
    }
    RestoreMeshFlags(f);
    current_ = nullptr;
    Logf("FORCE_GROOM off %s", f.label.c_str());
    return true;
}

Object* CurrentWorld() {
    Object* viewport = ue::At<Object*>(engine_, Off("/Script/Engine.Engine", "GameViewport"));
    return viewport ? ue::At<Object*>(viewport, Off("/Script/Engine.GameViewportClient", "World")) : nullptr;
}
bool PlayerContext(Object*& controller, Object*& pawn) {
    controller = pawn = nullptr;
    Object* world = CurrentWorld();
    Object* instance = world ? ue::At<Object*>(world, Off("/Script/Engine.World", "OwningGameInstance")) : nullptr;
    const Array players = instance ? ue::At<Array>(instance, Off("/Script/Engine.GameInstance", "LocalPlayers")) : Array{};
    Object* local = players.num > 0 ? static_cast<Object**>(players.data)[0] : nullptr;
    controller = local ? ue::At<Object*>(local, Off("/Script/Engine.Player", "PlayerController")) : nullptr;
    pawn = controller ? ue::At<Object*>(controller, Off("/Script/Engine.Controller", "Pawn")) : nullptr;
    return controller && ue::Alive(controller);
}
void UpdateStatus() {
    size_t grooms = 0, meshes = 0, npcs = 0; std::string player = "-";
    for (const Forced& f : forced_) {
        grooms += f.grooms.size(); meshes += f.meshes.size();
        if (f.appearance) player = f.label; else ++npcs;
    }
    SetStatus(L"Force Groom ON: %S | NPC %zu | Grooms %zu | mesh hair flagged %zu", player.c_str(), npcs, grooms, meshes);
}
void Refresh() {
    next_scan_ = GetTickCount64() + kScanInterval;
    stage_ = "refresh";
    Object* controller = nullptr; Object* pawn = nullptr;
    const bool player_context = PlayerContext(controller, pawn), external = ExternalOwnsPlayer();
    // A map change replaces every character; new ones may sit at old addresses.
    if (Object* world = CurrentWorld(); world && (world != world_ || !(ue::NameOf(world) == world_name_))) {
        if (world_) Logf("FORCE_GROOM world changed to %s; forcing its characters", Text(world).c_str());
        world_ = world; world_name_ = ue::NameOf(world); skipped_.clear();
    }
    for (size_t i = forced_.size(); i-- > 0;) if (!Same(forced_[i].actor, forced_[i].name)) forced_.erase(forced_.begin() + i);
    // An outfit change makes the game rebuild the head's Grooms from the normal (Groom-less)
    // row. Give the mesh hair back, then force again below from the new outfit's *_Seq row.
    // Grooms added to another character can go too (its blueprint destroyed them): force it again.
    for (size_t i = forced_.size(); i-- > 0;) {
        Forced& f = forced_[i];
        if (f.retiring_for_external) continue;
        if (external && f.actor == pawn) continue;
        bool removed = false;
        if (f.appearance) { CollectNative(f); removed = f.had_grooms && f.grooms.empty(); }
        else removed = std::none_of(f.grooms.begin(), f.grooms.end(), [](Object* g) { return ue::Alive(g); });
        if (removed) {
            RestoreMeshFlags(f);
            Logf("FORCE_GROOM game removed %s; reapplying", f.label.c_str());
            forced_.erase(forced_.begin() + i);
        }
    }
    for (size_t i = skipped_.size(); i-- > 0;) if (!Same(skipped_[i].actor, skipped_[i].name)) skipped_.erase(skipped_.begin() + i);
    if (!player_context) { SetStatus(L"Force Groom ON: no player world yet"); return; }
    Call all(ue::Default("/Script/Engine.Default__GameplayStatics"), "GetAllActorsOfClass");
    if (!all.Set("WorldContextObject", controller).Set("ActorClass", Cls("/Script/Engine.Character")).Invoke()) return;
    const Array actors = all.Get<Array>("OutActors");
    std::vector<Object*> list(static_cast<Object**>(actors.data), static_cast<Object**>(actors.data) + (actors.num > 0 ? actors.num : 0));
    ue::Free(actors.data);
    if (pawn && !external && !Known(pawn)) TryForce(pawn, true);
    for (Object* a : list) if (a && a != pawn && !Known(a)) TryForce(a, false);
    UpdateStatus();
    stage_ = "idle";
}
void SetOutfitStatus(const wchar_t* format, ...) {
    wchar_t text[200]{}; va_list args; va_start(args, format); vswprintf_s(text, format, args); va_end(args);
    AcquireSRWLockExclusive(&status_lock); wcscpy_s(outfit_status_, text); ReleaseSRWLockExclusive(&status_lock);
}
Object* PlayerAppearance() {
    Object* controller = nullptr; Object* pawn = nullptr;
    if (!PlayerContext(controller, pawn) || !pawn || !ue::Alive(pawn)) return nullptr;
    const std::vector<Object*> apps = Components(pawn, Cls("/Script/ProjectPBZ.PBZAppearanceComponent"));
    return apps.empty() ? nullptr : apps.front();
}
// Starts the native outfit system on the current player pawn when its blueprint left it off.
void EnsureOutfitLogic() {
    next_outfit_ = GetTickCount64() + 2000;
    outfit_disable_deferred_ = false;
    if (ExternalOwnsPlayer()) { SetOutfitStatus(L"Outfit unlock ON: paused while NPC owns player appearance"); return; }
    Object* app = PlayerAppearance();
    if (!app) { SetOutfitStatus(L"Outfit unlock ON: waiting for a player pawn"); return; }
    const int32_t logic = Off("/Script/ProjectPBZ.PBZAppearanceComponent", "DoPBZAppearanceLogic");
    const int32_t current = Off("/Script/ProjectPBZ.PBZAppearanceComponent", "CurrentOutfit");
    char owner[96]{}, outfit[96]{};
    ue::NameText(ue::NameOf(ue::ClassOf(ue::OuterOf(app))), owner, sizeof(owner));
    if (!ue::At<uint8_t>(app, logic)) {
        Call(app, "EnableAppearanceLogicAndInit").Invoke();
        ue::NameText(ue::At<Name>(app, current), outfit, sizeof(outfit));
        Logf("OUTFIT_UNLOCK enabled appearance logic on %s logic=%d currentOutfit=%s forceDefault=%d", owner,
            ue::At<uint8_t>(app, logic), outfit, ue::At<uint8_t>(app, Off("/Script/ProjectPBZ.PBZAppearanceComponent", "bForceDefaultAppearance")));
    }
    outfit_component_ = app;
    ue::NameText(ue::At<Name>(app, current), outfit, sizeof(outfit));
    SetOutfitStatus(L"Outfit unlock ON: %S logic=%d outfit=%S", owner, ue::At<uint8_t>(app, logic), outfit);
}
void DisableOutfitLogic() {
    if (ExternalOwnsPlayer()) { outfit_disable_deferred_ = true; SetOutfitStatus(L"Outfit unlock OFF: logic restore deferred until NPC releases player"); return; }
    outfit_disable_deferred_ = false;
    if (outfit_component_ && ue::Alive(outfit_component_)) {
        ue::At<uint8_t>(outfit_component_, Off("/Script/ProjectPBZ.PBZAppearanceComponent", "DoPBZAppearanceLogic")) = 0;
        Logf("OUTFIT_UNLOCK off; DoPBZAppearanceLogic cleared on the current pawn");
    }
    outfit_component_ = nullptr;
    SetOutfitStatus(L"Outfit unlock OFF (Shift+F6). Current look kept.");
}

// --- Credits ----------------------------------------------------------------------------
enum class Credits { Idle, Pushed, Rolling };
volatile LONG credits_request_ = 0;
Credits credits_ = Credits::Idle;
Object* credits_widget_ = nullptr;
ULONGLONG credits_deadline_ = 0, next_credits_scan_ = 0;
wchar_t credits_status_[160] = L"Credits: idle (Shift+F7)";
void SetCreditsStatus(const wchar_t* text) {
    AcquireSRWLockExclusive(&status_lock); wcscpy_s(credits_status_, text); ReleaseSRWLockExclusive(&status_lock);
}
void StartCredits() {
    Object* controller = nullptr; Object* pawn = nullptr;
    if (credits_ != Credits::Idle) { SetCreditsStatus(L"Credits: already playing"); return; }
    if (!PlayerContext(controller, pawn)) { SetCreditsStatus(L"Credits: no player controller yet"); return; }
    Call push(ue::Default("/Script/CommonGame.Default__AsyncAction_PushContentToLayerForPlayer"), "PushContentToLayerForPlayer");
    SoftPath soft;
    soft.package = ue::MakeName(L"/Game/PBZContent/UI/InGame/PersonnelList/WB_PersonnelList");
    soft.asset = ue::MakeName(L"WB_PersonnelList_C");
    unsigned char* slot = push.Slot("WidgetClass", sizeof(SoftPath));
    if (!slot || soft.package.None() || soft.asset.None()) { SetCreditsStatus(L"Credits: push function unavailable"); return; }
    std::memcpy(slot, &soft, sizeof(soft));
    push.Set("OwningPlayer", controller).Set("LayerName", ue::MakeName(L"UI.Layer.Menu")).Set("bSuspendInputUntilComplete", false);
    Object* action = push.Invoke() ? push.Get<Object*>("ReturnValue") : nullptr;
    if (!action) { SetCreditsStatus(L"Credits: push failed"); Logf("CREDITS push failed"); return; }
    Call(action, "Activate").Invoke();
    credits_ = Credits::Pushed; credits_deadline_ = GetTickCount64() + 15000;
    SetCreditsStatus(L"Credits: loading WB_PersonnelList");
    Logf("CREDITS pushed WB_PersonnelList onto UI.Layer.Menu (no FinishGame, no save)");
}
void TickCredits() {
    if (credits_ == Credits::Pushed) {
        if (GetTickCount64() < next_credits_scan_) return;
        next_credits_scan_ = GetTickCount64() + 250;
        Object* found = nullptr;
        ue::ForEachObject([&](Object* o) {
            if (!ue::IsDefault(o) && ue::NameIs(ue::NameOf(ue::ClassOf(o)), "WB_PersonnelList_C") && ue::Alive(o)) { found = o; return false; }
            return true;
        });
        if (found) { credits_widget_ = found; credits_ = Credits::Rolling; SetCreditsStatus(L"Credits: rolling (hold E skips and closes)"); }
        else if (GetTickCount64() > credits_deadline_) { credits_ = Credits::Idle; SetCreditsStatus(L"Credits: widget never appeared"); Logf("CREDITS widget did not appear"); }
        return;
    }
    if (credits_ != Credits::Rolling) return;
    bool activated = ue::Alive(credits_widget_);
    if (activated) { Call active(credits_widget_, "IsActivated"); activated = active.Invoke() && active.Get<bool>("ReturnValue"); }
    if (!activated) { credits_ = Credits::Idle; credits_widget_ = nullptr; SetCreditsStatus(L"Credits: closed"); return; }
    // Page 0 = credits roll, page 1 = settlement (WidgetSwitcher_0, switched by the widget's own animation).
    const int32_t switcher_offset = ue::Offset(ue::ClassOf(credits_widget_), "WidgetSwitcher_0");
    Object* switcher = switcher_offset >= 0 ? ue::At<Object*>(credits_widget_, switcher_offset) : nullptr;
    Call index(switcher, "GetActiveWidgetIndex");
    if (switcher && index.Invoke() && index.Get<int32_t>("ReturnValue") >= 1) {
        Call(credits_widget_, "DeactivateWidget").Invoke();
        credits_ = Credits::Idle; credits_widget_ = nullptr;
        SetCreditsStatus(L"Credits: finished, closed before the settlement page");
        Logf("CREDITS reached the settlement page; widget closed");
    }
}

void Activate() {
    InterlockedExchange(&active_, 1); skipped_.clear();
    Refresh();
    Logf("FORCE_GROOM active characters=%zu", forced_.size());
}
void Deactivate() {
    for (size_t i = forced_.size(); i-- > 0;) if (Release(forced_[i])) forced_.erase(forced_.begin() + i);
    skipped_.clear();
    InterlockedExchange(&active_, 0);
    SetStatus(forced_.empty() ? L"Force Groom OFF (F6). Added Grooms removed, mesh hair flags restored." : L"Force Groom OFF: player release still pending; ownership retained for retry.");
}
}

void Initialize(Logger logger, ExternalAppearanceOwned external_owned) { log_ = logger; external_owned_ = external_owned; }
void Request(Command command) { InterlockedExchange(&request_, command); }
bool Ready() { return InterlockedCompareExchange(&ready_, 0, 0) != 0; }
bool Active() { return InterlockedCompareExchange(&active_, 0, 0) != 0; }
void Status(wchar_t* out, size_t size) {
    AcquireSRWLockShared(&status_lock); wcsncpy_s(out, size, status_, _TRUNCATE); ReleaseSRWLockShared(&status_lock);
}
void RequestOutfitUnlock(bool on) { InterlockedExchange(&outfit_request_, on ? 1 : 0); }
bool OutfitUnlockActive() { return InterlockedCompareExchange(&outfit_active_, 0, 0) != 0; }
void OutfitStatus(wchar_t* out, size_t size) {
    AcquireSRWLockShared(&status_lock); wcsncpy_s(out, size, outfit_status_, _TRUNCATE); ReleaseSRWLockShared(&status_lock);
}
void RequestCredits() { InterlockedExchange(&credits_request_, 1); }
void CreditsStatus(wchar_t* out, size_t size) {
    AcquireSRWLockShared(&status_lock); wcsncpy_s(out, size, credits_status_, _TRUNCATE); ReleaseSRWLockShared(&status_lock);
}

namespace {
bool ReleaseRecordForExternal(Forced& f) {
        current_ = f.actor; stage_ = "player appearance release";
        if (f.groom_names.size() != f.grooms.size()) return false;
        const auto live_owned = [&](size_t at) { return Same(f.grooms[at], f.groom_names[at]); };
        bool live = false;
        for (size_t at = 0; at < f.grooms.size(); ++at) live = live || live_owned(at);
        if (live && f.appearance) {
            // Refresh only the same native owner/parent/mesh we forced. If a
            // normal outfit change replaced them, do not clear its new Grooms.
            if (!Same(f.appearance, f.appearance_name) || f.parents.empty() || !Same(f.parents.front(), f.player_parent_name) ||
                MeshOf(f.parents.front()) != f.player_parent_mesh) return false;
            Call refresh(f.appearance, "RefreshGroomComponents");
            if (!refresh.Set("ParentComponent", f.parents.front()).Set("GroomDataArray", Array{}).Invoke()) return false;
        } else if (live) {
            for (size_t at = 0; at < f.grooms.size(); ++at)
                if (live_owned(at) && !Call(f.grooms[at], "K2_DestroyComponent").Set("Object", f.grooms[at]).Invoke()) return false;
        }
        for (size_t at = 0; at < f.grooms.size(); ++at) if (live_owned(at)) return false;
        if (!RestoreMeshFlags(f)) return false;
        Logf("FORCE_GROOM retired only current player for external appearance: %s", f.label.c_str());
        current_ = nullptr; stage_ = "idle"; return true;
}
bool ReleasePlayerInternal() {
    if (!ue::Alive(engine_)) engine_ = ue::FindEngine();
    Object* controller = nullptr; Object* pawn = nullptr;
    if (!engine_ || !PlayerContext(controller, pawn) || !ue::Alive(pawn)) return forced_.empty();
    for (size_t i = forced_.size(); i-- > 0;) {
        Forced& f = forced_[i];
        if (f.actor != pawn || !Same(f.actor, f.name)) continue;
        f.retiring_for_external = true;
        if (!ReleaseRecordForExternal(f)) { current_ = nullptr; stage_ = "idle"; return false; }
        forced_.erase(forced_.begin() + i);
    }
    skipped_.erase(std::remove_if(skipped_.begin(), skipped_.end(), [&](const Seen& seen) { return seen.actor == pawn; }), skipped_.end());
    current_ = nullptr; stage_ = "idle"; next_scan_ = 0;
    return true;
}
}
bool ReleasePlayerForExternal() {
    __try { return ReleasePlayerInternal(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("FORCE_GROOM player-only release fault; retained record for retry"); current_ = nullptr; stage_ = "idle"; return false;
    }
}

void OnGameThread() {
    if (broken_) return;
    if (!validated_) {
        // Classes register during startup; retry every 2 s and only give up on a real mismatch.
        if (GetTickCount64() < next_validate_) return;
        next_validate_ = GetTickCount64() + 2000;
        if (!engine_) engine_ = ue::FindEngine();
        char why[160]{};
        if (!engine_ || !ue::FindObject("/Script/ProjectPBZ.PBZAppearanceStruct") || !ue::FindObject("/Script/HairStrandsCore.GroomComponent")) return;
        if (!ue::ValidateLayout(why, sizeof(why))) {
            broken_ = true; SetStatus(L"Force Groom disabled: %S", why); Logf("FORCE_GROOM disabled: %s", why); return;
        }
        validated_ = true; InterlockedExchange(&ready_, 1);
        SetStatus(L"Force Groom OFF (F6 loads Groom on the player and supported NPCs)");
        Logf("FORCE_GROOM layout validated");
    }
    stage_ = "force";
    const LONG command = InterlockedExchange(&request_, kNone);
    if (command == kToggle) { if (Active()) Deactivate(); else Activate(); }
    else if (command == kOff && Active()) Deactivate();
    else if (Active() && GetTickCount64() >= next_scan_) Refresh();
    stage_ = "outfit";
    const LONG outfit = InterlockedExchange(&outfit_request_, -1);
    if (outfit == 1) { InterlockedExchange(&outfit_active_, 1); EnsureOutfitLogic(); }
    else if (outfit == 0) { InterlockedExchange(&outfit_active_, 0); DisableOutfitLogic(); }
    else if (OutfitUnlockActive() && GetTickCount64() >= next_outfit_) EnsureOutfitLogic();
    if (outfit_disable_deferred_ && !ExternalOwnsPlayer()) DisableOutfitLogic();
    stage_ = "credits";
    if (InterlockedExchange(&credits_request_, 0)) StartCredits();
    TickCredits();
    stage_ = "idle";
}

void OnFault(DWORD code, uintptr_t address, DWORD faults, bool disabled) {
    char where[MAX_PATH + 32]{}, path[MAX_PATH]{};
    HMODULE module = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(address), &module) && GetModuleFileNameA(module, path, MAX_PATH)) {
        const char* file = std::strrchr(path, '\\');
        sprintf_s(where, "%s+0x%llx", file ? file + 1 : path, static_cast<unsigned long long>(address - reinterpret_cast<uintptr_t>(module)));
    } else {
        sprintf_s(where, "0x%llx", static_cast<unsigned long long>(address));
    }
    Object* actor = ue::Alive(current_) ? current_ : nullptr;
    Logf("FORCE_GROOM fault %lu/%lu: code 0x%08lx at %s during %s, actor %s", faults, GameThread::kMaxFaults, code, where, stage_,
        actor ? Text(actor).c_str() : "-");
    // Never retry what faulted: skip its actor, rebuild the donor cache, stop watching the credits.
    if (actor) skipped_.push_back({actor, ue::NameOf(actor)});
    current_ = nullptr; donors_.clear();
    if (!std::strcmp(stage_, "credits")) { credits_ = Credits::Idle; credits_widget_ = nullptr; SetCreditsStatus(L"Credits: stopped after a fault"); }
    stage_ = "idle";
    if (disabled) {
        broken_ = true; InterlockedExchange(&ready_, 0);
        SetStatus(L"Force Groom disabled after %lu faults (last 0x%08lx at %S); restart the game", faults, code, where);
        Logf("FORCE_GROOM disabled for this session after %lu faults", faults);
    } else {
        SetStatus(L"Force Groom recovered from a fault (see PBZDebugMenu.log); F6 still works");
    }
}
}
