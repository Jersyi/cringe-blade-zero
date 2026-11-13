// UUEPBS - shared state between the pose hook (game thread), the Lua
// bridge (its own thread) and the slider window (its own UI thread).
//
// Several characters can be edited at once. Each one ("actor") has its own bone and morph
// edits, keyed by the actor key Lua sends: "player" for the default character (the pawn you
// control or the profile's Target, so its sliders survive respawns), otherwise the actor's
// address. One actor is *active* - the one picked in the window; edits(), set_bone(),
// morphs(), ... work on it. Other actors keep their edits and their meshes stay tracked only
// while they have some (Lua is told which through edited_actors()).
#pragma once

#include "anim.hpp"
#include "mirror.hpp"
#include "morphs.hpp"
#include "sculpt.hpp"

#include <array>
#include <chrono>
#include <map>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace uuepbs
{
    // Where USkinnedMeshComponent keeps its double-buffered component-space pose.
    // Filled in by the hook installer (derived from the game's machine code).
    struct PoseLayout
    {
        uint32_t buffers{0};    // TArray<FTransform> ComponentSpaceTransformsArray[2]
        uint32_t read_index{0}; // int32 CurrentReadComponentTransforms
        uint32_t array_stride{16};
        uint32_t elem_size{96}; // 96 = UE5 FTransform (double), 48 = UE4 FTransform (float)
        bool valid() const { return buffers != 0 && read_index != 0 && (elem_size == 96 || elem_size == 48); }
    };

    // UE4 FTransform: three 16-byte float vectors.
    struct XformF
    {
        float rot[4];
        float pos[4];
        float scl[4];
    };
    static_assert(sizeof(XformF) == 48, "XformF must match UE4 FTransform layout");

    // Reads memory that may have been freed (addresses come from another thread).
    // Returns false instead of faulting. Tests use a plain memcpy.
    using SafeReader = bool (*)(uintptr_t address, void* out, size_t size);

    struct RawArray
    {
        void* data;
        int32_t num;
        int32_t max;
    };

    // Snapshot of the skeleton the window shows (taken from the primary rig).
    struct SkeletonView
    {
        std::vector<std::string> names;
        std::vector<int32_t> parents;
        std::vector<int32_t> depth;
        std::string label;
        std::string owner;
    };

    struct RigSummary
    {
        std::string label;
        std::string owner;
        int32_t bones{};
        uint64_t frames{};
        bool stale{};
        bool primary{};
        std::string actor; // actor key
        bool active{};     // belongs to the character picked in the window
    };

    // A character that has bone or morph edits.
    struct ActorSummary
    {
        std::string key;     // "player" or the actor's address
        std::string label;   // display name
        std::string pick_id; // what the picker uses to select it ("auto" for the player)
        size_t bones{};
        size_t morphs{};
        bool active{};
        std::string identity; // who the NPC is across reloads ("Anca"); empty = not remembered
    };

    // An NPC's sliders as they should be remembered (one file per identity).
    struct CharacterBook
    {
        std::string identity;
        std::string label;
        EditBook bones;
        MorphBook morphs;
    };

    constexpr const char* kPlayerActor = "player";

    class Registry
    {
      public:
        static constexpr size_t kMaxRigs = 64; // all tracked meshes of all kept characters

        static Registry& instance();

        void set_safe_reader(SafeReader reader) { m_read = reader; }

        // ---- bridge side ----------------------------------------------------
        // `primary` rigs feed the bone list shown in the window. `owner` is the actor's display name.
        // `actor` is the key of the character the mesh belongs to; `primary` = that character's main mesh.
        bool track(uintptr_t component, const std::string& label, const std::string& owner, std::vector<std::string> names,
                   std::vector<int32_t> parents, bool primary, std::string& message, const std::string& actor = kPlayerActor);
        // Parent-relative reference pose of a tracked rig (for measuring left/right mirroring).
        void set_reference_pose(uintptr_t component, std::vector<Xform> local_pose);
        std::vector<uintptr_t> tracked_components() const;
        uintptr_t primary_component() const;
        void untrack(uintptr_t component);
        void untrack_all();
        bool is_tracked(uintptr_t component) const;

        // ---- hook side (game thread) ----------------------------------------
        void set_layout(const PoseLayout& layout);
        PoseLayout layout() const;
        void on_pose_finalized(void* component);

        // ---- characters --------------------------------------------------------
        // The character the window edits. Rebuilds the skeleton view when it changes.
        void set_active_actor(const std::string& key, const std::string& label, const std::string& pick_id);
        std::string active_actor() const;
        // Characters with any bone or morph edits (the active one included), active first.
        std::vector<ActorSummary> edited_actors() const;
        // Drops a character's edits (its meshes are released once Lua hears of it).
        // forget_memory: also forget the remembered identity (Release / reset all). A character that
        // merely despawned keeps its memory, so its sliders come back when it appears again.
        void forget_actor(const std::string& key, bool forget_memory = true);
        // NPC identity across reloads (name without instance number, or class@face mesh).
        void set_actor_identity(const std::string& key, const std::string& identity);
        // Current sliders of every NPC that has an identity (empty books included).
        std::vector<CharacterBook> character_books() const;
        // Identities whose memory was dropped since the last call (their files get deleted).
        std::vector<std::string> take_forgotten_identities();
        // Identities with a saved file (shown in the window).
        void set_remembered(std::vector<std::string> identities);
        std::vector<std::string> remembered() const;
        EditBook edits_of(const std::string& key) const;
        void replace_edits_of(const std::string& key, EditBook book);
        MorphBook morphs_of(const std::string& key) const;
        void replace_morphs_of(const std::string& key, MorphBook book);
        // Every character's morph weights (what Lua applies), keyed by actor key.
        std::map<std::string, MorphBook> all_morphs(uint64_t* revision = nullptr) const;

        // ---- UI / preset side (the active character) ---------------------------
        EditBook edits(uint64_t* revision = nullptr) const;
        // Changes whenever any character's edits change or another character becomes active.
        uint64_t edit_revision() const { return m_edit_revision.load(std::memory_order_acquire); }
        void set_bone(const std::string& bone, const BoneEdit& scale);
        void clear_bone(const std::string& bone);
        void replace_edits(EditBook book);
        void clear_edits();

        bool enabled() const { return m_enabled.load(std::memory_order_relaxed); }
        void set_enabled(bool on); // also bumps the morph revision (morphs are sent to Lua only while enabled)

        // ---- Locomotion state layer (per context, per idle/walk/run/sprint) ------
        // A context is "unarmed" (weapon on the back) or a weapon key when it is drawn; every
        // context keeps its own four state books. The layer is added on top of the normal sliders.
        static constexpr int kLocoStateCount = uuepbs::kLocoStateCount;
        LocoBookMap loco_all(uint64_t* revision = nullptr) const;
        std::vector<std::string> loco_contexts(uint64_t* revision = nullptr) const; // known, ordered
        uint64_t loco_revision() const { return m_loco_revision.load(std::memory_order_acquire); }
        void set_loco_bone(const std::string& context, int state, const std::string& bone, const BoneEdit& edit);
        void clear_loco_bone(const std::string& context, int state, const std::string& bone);
        void replace_loco_book(const std::string& context, int state, EditBook book);
        void replace_loco_all(LocoBookMap books);
        void remember_loco_context(const std::string& context);
        // Live context/state reported by Lua. layer_on = the layer should be shown right now
        // (weapon context is valid and no action animation is playing).
        void set_loco_live(const std::string& context, int state, bool layer_on);
        std::string loco_live_context() const;
        int loco_live_state() const;
        bool loco_layer_on() const;
        // 过场（cinematic）检测结果：Lua 报上来给状态页显示，不影响叠加本身。
        void set_cinematic(bool on) { m_cinematic.store(on, std::memory_order_relaxed); }
        bool cinematic() const { return m_cinematic.load(std::memory_order_relaxed); }
        // Preview: while the Locomotion tab is open, force this context/state on regardless of
        // what the player is doing, so the sliders can be tuned while standing still.
        // Empty context or state < 0 = off.
        void set_loco_preview(const std::string& context, int state);
        // Cross-fade time used when the context, the state or the on/off flag changes (0 = snap).
        void set_loco_blend_ms(int ms) { m_loco_blend_ms.store(ms < 0 ? 0 : (ms > 5000 ? 5000 : ms), std::memory_order_relaxed); }
        int loco_blend_ms() const { return m_loco_blend_ms.load(std::memory_order_relaxed); }

        // ---- 程序化时间轴动画（眨眼这类小动作，独立文件 Animations\<名字>.json） ----
        // 只叠加到玩家身上，加在滑条和 Locomotion 层之上；每条轨道的节点顺序由 UI 编辑。
        struct AnimLiveTrack
        {
            int node{-1};       // 当前节点
            double progress{0}; // 节点内进度 0..1
            bool finished{false};
        };
        std::vector<Animation> animations() const;
        void set_animations(std::vector<Animation> animations);
        uint64_t anim_revision() const { return m_anim_revision.load(std::memory_order_acquire); }
        void set_anim_playing(bool on) { m_anim_playing.store(on, std::memory_order_relaxed); }
        bool anim_playing() const { return m_anim_playing.load(std::memory_order_relaxed); }
        void restart_animations();
        // UI 用：每条轨道此刻的节点与进度（外层下标与 animations() 一一对应）。
        std::vector<std::vector<AnimLiveTrack>> anim_live();

        // ---- morph targets (weights applied by the Lua script) ------------------
        MorphBook morphs(uint64_t* revision = nullptr) const;
        uint64_t morph_revision() const { return m_morph_revision.load(std::memory_order_acquire); }
        void set_morph(const std::string& morph, double weight); // present = overrides the game's value
        void clear_morph(const std::string& morph);              // back to the game's own value
        void replace_morphs(MorphBook book);
        void clear_morphs();
        // Morph names found on the tracked meshes (primary mesh first, no duplicates).
        void set_morph_names(std::vector<std::string> names);
        std::vector<std::string> morph_names(uint64_t* revision = nullptr) const;
        uint64_t morph_names_revision() const { return m_morph_names_revision.load(std::memory_order_acquire); }
        // Morphs that something else keeps setting (reported by Lua): folded names.
        void set_animated_morphs(std::set<std::string> folded);
        std::set<std::string> animated_morphs() const;

        SkeletonView skeleton(uint64_t* revision = nullptr) const;
        MirrorTable mirror_table() const;
        std::string mirror_source() const;
        BoneEdit mirror_edit(const std::string& bone, const BoneEdit& edit) const;
        uint64_t skeleton_revision() const { return m_skeleton_revision.load(std::memory_order_acquire); }
        std::vector<RigSummary> rigs() const;

        // A rescan is wanted when a tracked mesh changed or went away, or the user asked.
        void request_rescan() { m_rescan.store(true, std::memory_order_release); }
        bool take_rescan_request() { return m_rescan.exchange(false, std::memory_order_acq_rel); }

      private:
        struct Actor
        {
            std::string label;
            std::string pick_id;
            std::string identity;
            EditBook book;
            uint64_t revision{}; // value of m_edit_revision at its last change (unique across actors)
        };

        struct Rig
        {
            uintptr_t component{};
            uintptr_t vtable{};
            int32_t object_index{};
            uintptr_t object_class{};
            std::string label;
            std::string owner;
            std::string actor_key;
            Actor* actor{}; // node in m_actors (stable while the rig exists)
            bool primary{};
            bool stale{};
            uint64_t frames{};
            std::vector<Xform> reference; // parent-relative reference pose, when Lua could read it
            RigSculptor sculptor;
            std::vector<Xform> scratch; // float poses are converted here
            std::vector<int32_t> inputs; // bones converted for the current frame
        };

        bool peek_raw(uintptr_t address, void* out, size_t size) const;
        void measure_mirror_locked(const Rig& rig, const std::vector<Xform>& pose, bool local, const char* source);

        Rig* find_rig(uintptr_t component);
        const Rig* find_rig(uintptr_t component) const;
        void rebuild_hot_list();
        void rebuild_skeleton_view();
        void bump_edits(Actor& actor);
        Actor& actor_locked(const std::string& key);
        Actor& active_locked() { return actor_locked(m_active); }
        void prune_actors_locked();

        mutable std::mutex m_lock;
        std::vector<std::unique_ptr<Rig>> m_rigs;
        std::array<std::atomic<uintptr_t>, kMaxRigs> m_hot{};
        std::atomic<size_t> m_hot_count{0}; // used slots of m_hot (the detour's fast reject)

        std::map<std::string, Actor> m_actors; // guarded by m_lock
        std::vector<std::string> m_forgotten;  // guarded by m_lock
        std::vector<std::string> m_remembered; // guarded by m_lock
        std::string m_active{kPlayerActor};
        EditBook m_empty_book;
        std::atomic<uint64_t> m_edit_revision{1};
        std::atomic<bool> m_enabled{true};

        // locomotion layer: guarded by m_lock
        LocoBookMap m_loco_books;
        std::vector<std::string> m_loco_known; // ordered context keys for the UI
        std::string m_loco_live_context{"unarmed"};
        int m_loco_live_state{-1};
        bool m_loco_layer_on{false};
        std::string m_loco_preview_context;
        int m_loco_preview_state{-1};
        std::atomic<uint64_t> m_loco_revision{1};
        std::atomic<int> m_loco_blend_ms{250};
        std::atomic<bool> m_cinematic{false};

        // Cross-fade bookkeeping (guarded by m_lock): the layer actually applied each frame is
        // m_loco_layer, faded from m_loco_fade_from towards the current target book.
        EditBook m_loco_layer;
        EditBook m_loco_fade_from;
        uint64_t m_loco_target_revision{0};
        std::string m_loco_target_context;
        int m_loco_target_state{-1};
        bool m_loco_target_on{false};
        bool m_loco_fade_active{false};
        int m_loco_fade_ms{250};
        std::chrono::steady_clock::time_point m_loco_fade_start{};
        int64_t m_loco_layer_ms{-1};
        uint64_t m_loco_layer_serial{0};
        void update_loco_layer_locked();

        SkeletonView m_view;
        std::atomic<uint64_t> m_skeleton_revision{0};

        // 时间轴动画：数据 + 运行时（都受 m_lock 保护）+ 每帧推进出来的叠加层
        std::vector<Animation> m_anims;
        std::vector<AnimRuntime> m_anim_runtimes;
        EditBook m_anim_layer;
        uint64_t m_anim_layer_serial{0};
        std::atomic<uint64_t> m_anim_revision{1};
        std::atomic<bool> m_anim_playing{true};
        std::chrono::steady_clock::time_point m_anim_last_tick{};
        void update_anim_layer_locked();

        PoseLayout m_layout{};
        std::atomic<bool> m_rescan{false};

        mutable std::mutex m_morph_lock; // separate from m_lock: the pose hook never waits on morph edits
        std::map<std::string, MorphBook> m_morph_books; // by actor key
        std::string m_morph_active{kPlayerActor};       // copy of m_active for the morph side
        std::atomic<uint64_t> m_morph_revision{1};
        std::vector<std::string> m_morph_names;
        std::atomic<uint64_t> m_morph_names_revision{1};
        std::set<std::string> m_animated_morphs;

        MirrorTable m_mirror{};
        std::string m_mirror_source{"not measured yet"};
        bool m_mirror_from_reference{false};
        SafeReader m_read{};
    };
} // namespace uuepbs
