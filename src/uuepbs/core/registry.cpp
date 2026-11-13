#include "registry.hpp"
#include "../ui/i18n.hpp"

#include <algorithm>
#include <cstring>

namespace uuepbs
{
    namespace
    {
        // UObjectBase members used to make sure a tracked address still holds the same
        // component (the allocator may hand a freed component's memory to a new one).
        constexpr uintptr_t kObjIndexOffset = 0x0C; // int32 InternalIndex
        constexpr uintptr_t kObjClassOffset = 0x10; // UClass* ClassPrivate
        // (same in UE4 and UE5)

        template <typename T> T peek(uintptr_t address)
        {
            T value;
            std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
            return value;
        }

        constexpr uint64_t kDisabledBit = 1ull << 63;

        // Frames to wait before measuring mirroring from a live pose (lets the idle animation settle).
        constexpr uint64_t kLiveMirrorFrame = 30;

        // Locomotion layer cross-fade: per-bone rotation/move are interpolated, scale is lerped
        // between the two values (1.0 = unchanged), and the discrete "children" mode follows the
        // side that is currently dominant.
        BoneEdit lerp_edit(const BoneEdit& from, const BoneEdit& to, double t)
        {
            BoneEdit e = to;
            for (int a = 0; a < 3; ++a)
            {
                e.axis[a] = from.axis[a] + (to.axis[a] - from.axis[a]) * t;
                e.turn[a] = from.turn[a] + (to.turn[a] - from.turn[a]) * t;
                e.shift[a] = from.shift[a] + (to.shift[a] - from.shift[a]) * t;
            }
            e.spread = t < 0.5 ? from.spread : to.spread;
            e.clamp();
            return e;
        }

        EditBook blend_books(const EditBook& from, const EditBook& to, double t)
        {
            if (t <= 0.0)
            {
                return from;
            }
            if (t >= 1.0)
            {
                return to;
            }
            const BoneEdit neutral;
            EditBook out = to;
            for (auto& [key, entry] : out)
            {
                const auto it = from.find(key);
                entry.edit = lerp_edit(it != from.end() ? it->second.edit : neutral, entry.edit, t);
            }
            for (const auto& [key, entry] : from)
            {
                if (out.find(key) != out.end())
                {
                    continue;
                }
                BoneEdit e = lerp_edit(entry.edit, neutral, t);
                if (!e.is_neutral())
                {
                    out[key] = EditEntry{entry.bone, e};
                }
            }
            return out;
        }

        // 把一层叠加到合并好的书上：旋转和位移相加，缩放相乘（和 Locomotion 层一致）。
        void add_layer(EditBook& merged, const EditBook& layer)
        {
            for (const auto& [key, entry] : layer)
            {
                const auto it = merged.find(key);
                if (it == merged.end())
                {
                    merged.emplace(key, entry);
                    continue;
                }
                BoneEdit edit = it->second.edit;
                for (int a = 0; a < 3; ++a)
                {
                    edit.axis[a] *= entry.edit.axis[a];
                    edit.turn[a] += entry.edit.turn[a];
                    edit.shift[a] += entry.edit.shift[a];
                }
                edit.clamp();
                it->second.edit = edit;
            }
        }
    } // namespace

    bool Registry::peek_raw(uintptr_t address, void* out, size_t size) const
    {
        if (m_read)
        {
            return m_read(address, out, size);
        }
        std::memcpy(out, reinterpret_cast<const void*>(address), size);
        return true;
    }

    void Registry::measure_mirror_locked(const Rig& rig, const std::vector<Xform>& pose, bool local, const char* source)
    {
        MirrorTable table = build_mirror_table(rig.sculptor.names(), rig.sculptor.parents(), pose, local);
        if (table.centre_axis < 0)
        {
            m_mirror_source = std::string(ui::tr("could not measure from ")) + source + ui::tr(" (no left/right bone pairs found)");
            return;
        }
        static const char* axis_names[3] = {"X", "Y", "Z"};
        m_mirror_source = std::string(ui::tr("measured from ")) + source + ": " + std::to_string(table.pairs) + ui::tr(" pairs across ") +
                          axis_names[table.centre_axis] + (table.inexact ? ", " + std::to_string(table.inexact) + ui::tr(" uncertain") : "");
        m_mirror = std::move(table);
    }

    Registry& Registry::instance()
    {
        static Registry registry;
        return registry;
    }

    Registry::Rig* Registry::find_rig(uintptr_t component)
    {
        for (auto& rig : m_rigs)
        {
            if (rig->component == component)
            {
                return rig.get();
            }
        }
        return nullptr;
    }

    const Registry::Rig* Registry::find_rig(uintptr_t component) const
    {
        for (const auto& rig : m_rigs)
        {
            if (rig->component == component)
            {
                return rig.get();
            }
        }
        return nullptr;
    }

    void Registry::rebuild_hot_list()
    {
        size_t i = 0;
        for (; i < m_rigs.size() && i < kMaxRigs; ++i)
        {
            m_hot[i].store(m_rigs[i]->component, std::memory_order_release);
        }
        const size_t used = i;
        for (; i < kMaxRigs; ++i)
        {
            m_hot[i].store(0, std::memory_order_release);
        }
        m_hot_count.store(used, std::memory_order_release);
    }

    void Registry::rebuild_skeleton_view()
    {
        // The window lists the bones of the active character's main mesh.
        const Rig* source = nullptr;
        for (const auto& rig : m_rigs)
        {
            if (rig->primary && !rig->stale && rig->actor_key == m_active)
            {
                source = rig.get();
                break;
            }
        }
        if (!source)
        {
            for (const auto& rig : m_rigs)
            {
                if (!rig->stale && rig->actor_key == m_active && (!source || rig->sculptor.bone_count() > source->sculptor.bone_count()))
                {
                    source = rig.get();
                }
            }
        }

        SkeletonView view;
        if (source)
        {
            view.names = source->sculptor.names();
            view.parents = source->sculptor.parents();
            view.depth.assign(view.names.size(), 0);
            for (size_t i = 0; i < view.parents.size(); ++i)
            {
                const int32_t p = view.parents[i];
                view.depth[i] = p >= 0 ? view.depth[p] + 1 : 0;
            }
            view.label = source->label;
            view.owner = source->owner;
        }
        if (view.names != m_view.names)
        {
            m_mirror = {};
            m_mirror_from_reference = false;
            m_mirror_source = view.names.empty() ? ui::tr("not measured yet") : ui::tr("waiting for a pose to measure");
        }
        if (m_mirror.centre_axis < 0 && source && !source->reference.empty() && source->reference.size() == view.names.size())
        {
            // Another character became active (or came back): measure from its reference pose right away.
            measure_mirror_locked(*source, source->reference, true, "the reference pose");
            m_mirror_from_reference = m_mirror.centre_axis >= 0;
        }
        m_view = std::move(view);
        m_skeleton_revision.fetch_add(1, std::memory_order_acq_rel);
    }

    bool Registry::track(uintptr_t component, const std::string& label, const std::string& owner, std::vector<std::string> names,
                         std::vector<int32_t> parents, bool primary, std::string& message, const std::string& actor)
    {
        if (component == 0)
        {
            message = "null component";
            return false;
        }
        uintptr_t vtable = 0;
        int32_t object_index = 0;
        uintptr_t object_class = 0;
        if (!peek_raw(component, &vtable, sizeof(vtable)) || !peek_raw(component + kObjIndexOffset, &object_index, sizeof(object_index)) ||
            !peek_raw(component + kObjClassOffset, &object_class, sizeof(object_class)) || vtable == 0)
        {
            message = label + ": component memory is not readable";
            return false;
        }

        std::lock_guard guard(m_lock);
        Rig* rig = find_rig(component);
        if (!rig)
        {
            if (m_rigs.size() >= kMaxRigs)
            {
                message = "too many tracked meshes";
                return false;
            }
            m_rigs.push_back(std::make_unique<Rig>());
            rig = m_rigs.back().get();
        }

        // Re-registering the same skeleton keeps the per-buffer memory intact.
        const bool unchanged = rig->component == component && rig->sculptor.names() == names && rig->sculptor.parents() == parents;
        std::string why;
        if (!unchanged && !rig->sculptor.set_skeleton(std::move(names), std::move(parents), &why))
        {
            m_rigs.erase(std::remove_if(m_rigs.begin(), m_rigs.end(),
                                        [&](const auto& r) {
                                            return r.get() == rig;
                                        }),
                         m_rigs.end());
            rebuild_hot_list();
            prune_actors_locked();
            message = why;
            return false;
        }

        rig->component = component;
        rig->vtable = vtable;
        rig->object_index = object_index;
        rig->object_class = object_class;
        rig->label = label;
        rig->owner = owner;
        const std::string key = actor.empty() ? std::string(kPlayerActor) : actor;
        if (rig->actor_key != key)
        {
            rig->reference.clear();
        }
        rig->actor_key = key;
        rig->actor = &actor_locked(key);
        rig->primary = primary;
        rig->stale = false;
        rig->frames = 0;
        prune_actors_locked();

        rebuild_hot_list();
        rebuild_skeleton_view();
        message = "tracking " + label + " (" + std::to_string(rig->sculptor.bone_count()) + " bones)";
        return true;
    }

    void Registry::set_reference_pose(uintptr_t component, std::vector<Xform> local_pose)
    {
        std::lock_guard guard(m_lock);
        Rig* rig = find_rig(component);
        if (!rig || local_pose.size() != static_cast<size_t>(rig->sculptor.bone_count()))
        {
            return;
        }
        rig->reference = std::move(local_pose); // kept for when this character becomes active
        if (m_view.names != rig->sculptor.names() || rig->actor_key != m_active)
        {
            return;
        }
        measure_mirror_locked(*rig, rig->reference, true, "the reference pose");
        m_mirror_from_reference = m_mirror.centre_axis >= 0;
    }

    std::vector<uintptr_t> Registry::tracked_components() const
    {
        std::lock_guard guard(m_lock);
        std::vector<uintptr_t> out;
        for (const auto& rig : m_rigs)
        {
            if (!rig->stale)
            {
                out.push_back(rig->component);
            }
        }
        return out;
    }

    uintptr_t Registry::primary_component() const
    {
        std::lock_guard guard(m_lock);
        uintptr_t best = 0;
        int32_t bones = -1;
        for (const auto& rig : m_rigs)
        {
            if (rig->stale || rig->actor_key != m_active)
            {
                continue;
            }
            if (rig->primary)
            {
                return rig->component;
            }
            if (rig->sculptor.bone_count() > bones)
            {
                bones = rig->sculptor.bone_count();
                best = rig->component;
            }
        }
        return best;
    }

    void Registry::untrack(uintptr_t component)
    {
        std::lock_guard guard(m_lock);
        const auto before = m_rigs.size();
        m_rigs.erase(std::remove_if(m_rigs.begin(), m_rigs.end(),
                                    [&](const auto& r) {
                                        return r->component == component;
                                    }),
                     m_rigs.end());
        if (m_rigs.size() != before)
        {
            rebuild_hot_list();
            rebuild_skeleton_view();
            prune_actors_locked();
        }
    }

    void Registry::untrack_all()
    {
        std::lock_guard guard(m_lock);
        m_rigs.clear();
        rebuild_hot_list();
        rebuild_skeleton_view();
        prune_actors_locked();
    }

    bool Registry::is_tracked(uintptr_t component) const
    {
        std::lock_guard guard(m_lock);
        const Rig* rig = find_rig(component);
        return rig && !rig->stale;
    }

    void Registry::set_layout(const PoseLayout& layout)
    {
        std::lock_guard guard(m_lock);
        m_layout = layout;
    }

    PoseLayout Registry::layout() const
    {
        std::lock_guard guard(m_lock);
        return m_layout;
    }

    void Registry::on_pose_finalized(void* component)
    {
        // Runs for every skeletal mesh in the game, every frame: reject untracked ones
        // with a few plain loads before touching the lock.
        const size_t used = m_hot_count.load(std::memory_order_acquire);
        if (used == 0)
        {
            return;
        }
        const uintptr_t self = reinterpret_cast<uintptr_t>(component);
        bool hot = false;
        for (size_t i = 0; i < used; ++i)
        {
            if (m_hot[i].load(std::memory_order_relaxed) == self)
            {
                hot = true;
                break;
            }
        }
        if (!hot)
        {
            return;
        }

        std::lock_guard guard(m_lock);
        Rig* rig = find_rig(self);
        if (!rig || rig->stale || !m_layout.valid())
        {
            return;
        }

        // Same object as when Lua registered it?
        if (peek<uintptr_t>(self) != rig->vtable || peek<int32_t>(self + kObjIndexOffset) != rig->object_index ||
            peek<uintptr_t>(self + kObjClassOffset) != rig->object_class)
        {
            rig->stale = true;
            rebuild_skeleton_view();
            request_rescan();
            return;
        }

        const int32_t read = peek<int32_t>(self + m_layout.read_index);
        if (read != 0 && read != 1)
        {
            return;
        }
        const RawArray pose = peek<RawArray>(self + m_layout.buffers + static_cast<uintptr_t>(read) * m_layout.array_stride);
        if (!pose.data || pose.num <= 0)
        {
            return;
        }
        if (pose.num != rig->sculptor.bone_count())
        {
            // Mesh swapped under us: stop touching it until Lua re-registers the bones.
            rig->stale = true;
            rig->sculptor.forget_buffers();
            rebuild_skeleton_view();
            request_rescan();
            return;
        }

        // Each mesh is sculpted with its own character's edits. The player's rigs also get the
        // locomotion layer (the cross-faded state book) and the timeline animation layer added on top.
        update_loco_layer_locked();
        update_anim_layer_locked();
        const bool on = m_enabled.load(std::memory_order_relaxed);
        const bool is_player = rig->actor_key == kPlayerActor;
        const bool loco_active = is_player && !m_loco_layer.empty();
        const bool anim_active = is_player && !m_anim_layer.empty();
        uint64_t wanted = rig->actor->revision;
        if (loco_active)
        {
            // The serial changes while the cross-fade runs, so every blended frame re-plans the bones.
            wanted ^= (m_loco_layer_serial * 1000003ull) ^ (static_cast<uint64_t>(m_loco_target_state + 1) * 131ull);
        }
        if (anim_active)
        {
            // 动画每帧都在变，序列号让骨骼规划跟着重算。
            wanted ^= (m_anim_layer_serial * 2000003ull);
        }
        wanted |= (on ? 0 : kDisabledBit);
        if (rig->sculptor.bound_revision() != wanted)
        {
            if (on && (loco_active || anim_active))
            {
                EditBook merged = rig->actor->book;
                if (loco_active)
                {
                    add_layer(merged, m_loco_layer);
                }
                if (anim_active)
                {
                    add_layer(merged, m_anim_layer);
                }
                rig->sculptor.bind(merged, wanted);
            }
            else
            {
                rig->sculptor.bind(on ? rig->actor->book : m_empty_book, wanted);
            }
        }
        // No reference pose from Lua: measure mirroring from an early live frame instead
        // (before this frame's edits are applied).
        if (rig->frames + 1 == kLiveMirrorFrame && !m_mirror_from_reference && m_mirror.centre_axis < 0 && m_view.names == rig->sculptor.names())
        {
            std::vector<Xform> live(static_cast<size_t>(pose.num));
            for (int32_t i = 0; i < pose.num; ++i)
            {
                if (m_layout.elem_size == 96)
                {
                    live[i] = static_cast<const Xform*>(pose.data)[i];
                }
                else
                {
                    const XformF& f = static_cast<const XformF*>(pose.data)[i];
                    for (int a = 0; a < 4; ++a)
                    {
                        live[i].rot[a] = f.rot[a];
                        live[i].pos[a] = f.pos[a];
                        live[i].scl[a] = f.scl[a];
                    }
                }
            }
            measure_mirror_locked(*rig, live, false, "a live pose");
        }

        ++rig->frames;
        if (!rig->sculptor.has_work(pose.data))
        {
            return; // no sliders moved and nothing of ours to undo: leave the pose alone
        }
        if (m_layout.elem_size == 96)
        {
            rig->sculptor.set_quantize(false);
            rig->sculptor.apply(static_cast<Xform*>(pose.data), pose.num);
        }
        else
        {
            // UE4: widen the bones the sculptor reads to double, sculpt, write back the ones it
            // changed. Unchanged bones round-trip exactly; untouched bones are never converted.
            auto* floats = static_cast<XformF*>(pose.data);
            rig->scratch.resize(static_cast<size_t>(pose.num));
            rig->sculptor.gather_inputs(pose.data, rig->inputs);
            for (const int32_t i : rig->inputs)
            {
                for (int a = 0; a < 4; ++a)
                {
                    rig->scratch[i].rot[a] = floats[i].rot[a];
                    rig->scratch[i].pos[a] = floats[i].pos[a];
                    rig->scratch[i].scl[a] = floats[i].scl[a];
                }
            }
            rig->sculptor.set_quantize(true);
            if (rig->sculptor.apply(rig->scratch.data(), pose.num, pose.data) > 0)
            {
                for (const int32_t i : rig->sculptor.touched())
                {
                    for (int a = 0; a < 4; ++a)
                    {
                        floats[i].rot[a] = static_cast<float>(rig->scratch[i].rot[a]);
                        floats[i].pos[a] = static_cast<float>(rig->scratch[i].pos[a]);
                        floats[i].scl[a] = static_cast<float>(rig->scratch[i].scl[a]);
                    }
                }
            }
        }
    }

    void Registry::bump_edits(Actor& actor)
    {
        actor.revision = m_edit_revision.fetch_add(1, std::memory_order_acq_rel) + 1;
    }

    Registry::Actor& Registry::actor_locked(const std::string& key)
    {
        auto it = m_actors.find(key);
        if (it == m_actors.end())
        {
            it = m_actors.emplace(key, Actor{}).first;
            it->second.label = key == kPlayerActor ? "Player" : key;
            it->second.pick_id = key == kPlayerActor ? "auto" : key;
            bump_edits(it->second); // a revision no other actor has
        }
        return it->second;
    }

    // Characters with no edits, no meshes and not active are forgotten.
    void Registry::prune_actors_locked()
    {
        for (auto it = m_actors.begin(); it != m_actors.end();)
        {
            const bool used = it->first == m_active || it->first == kPlayerActor || !it->second.book.empty() ||
                              std::any_of(m_rigs.begin(), m_rigs.end(), [&](const auto& r) { return r->actor == &it->second; });
            it = used ? std::next(it) : m_actors.erase(it);
        }
    }

    void Registry::set_active_actor(const std::string& key_in, const std::string& label, const std::string& pick_id)
    {
        const std::string key = key_in.empty() ? std::string(kPlayerActor) : key_in;
        {
            std::lock_guard guard(m_lock);
            Actor& a = actor_locked(key);
            if (!label.empty())
            {
                a.label = label;
            }
            if (!pick_id.empty())
            {
                a.pick_id = pick_id;
            }
            if (m_active == key)
            {
                return;
            }
            m_active = key;
            m_edit_revision.fetch_add(1, std::memory_order_acq_rel); // the window re-reads edits()
            rebuild_skeleton_view();
            prune_actors_locked();
        }
        std::lock_guard morph_guard(m_morph_lock);
        m_morph_active = key;
        m_morph_revision.fetch_add(1, std::memory_order_acq_rel);
        m_morph_names_revision.fetch_add(1, std::memory_order_acq_rel);
    }

    std::string Registry::active_actor() const
    {
        std::lock_guard guard(m_lock);
        return m_active;
    }

    std::vector<ActorSummary> Registry::edited_actors() const
    {
        std::vector<ActorSummary> out;
        std::map<std::string, size_t> morph_counts;
        {
            std::lock_guard morph_guard(m_morph_lock);
            for (const auto& [key, book] : m_morph_books)
            {
                if (!book.empty())
                {
                    morph_counts[key] = book.size();
                }
            }
        }
        std::lock_guard guard(m_lock);
        for (const auto& [key, a] : m_actors)
        {
            const auto m = morph_counts.find(key);
            const size_t morphs = m == morph_counts.end() ? 0 : m->second;
            if (a.book.empty() && morphs == 0)
            {
                continue;
            }
            out.push_back({key, a.label, a.pick_id, a.book.size(), morphs, key == m_active, a.identity});
        }
        // morphs only, character not known to the bone side (should not happen, but keep them)
        for (const auto& [key, n] : morph_counts)
        {
            if (!m_actors.count(key))
            {
                out.push_back({key, key == kPlayerActor ? "Player" : key, key == kPlayerActor ? "auto" : key, 0, n, key == m_active, {}});
            }
        }
        std::stable_partition(out.begin(), out.end(), [](const ActorSummary& s) { return s.active; });
        return out;
    }

    void Registry::forget_actor(const std::string& key, bool forget_memory)
    {
        {
            std::lock_guard guard(m_lock);
            const auto it = m_actors.find(key);
            if (it != m_actors.end())
            {
                if (!it->second.identity.empty())
                {
                    if (forget_memory)
                    {
                        m_forgotten.push_back(it->second.identity);
                    }
                    it->second.identity.clear(); // either way this actor no longer stands for that character
                }
                if (!it->second.book.empty())
                {
                    it->second.book.clear();
                    bump_edits(it->second);
                }
            }
            prune_actors_locked();
        }
        std::lock_guard morph_guard(m_morph_lock);
        if (m_morph_books.erase(key) > 0)
        {
            m_morph_revision.fetch_add(1, std::memory_order_acq_rel);
        }
    }

    void Registry::set_actor_identity(const std::string& key, const std::string& identity)
    {
        if (key.empty() || key == kPlayerActor)
        {
            return;
        }
        std::lock_guard guard(m_lock);
        actor_locked(key).identity = identity;
    }

    std::vector<CharacterBook> Registry::character_books() const
    {
        std::vector<CharacterBook> out;
        std::vector<std::string> keys;
        {
            std::lock_guard guard(m_lock);
            for (const auto& [key, a] : m_actors)
            {
                if (key != kPlayerActor && !a.identity.empty())
                {
                    out.push_back({a.identity, a.label, a.book, {}});
                    keys.push_back(key);
                }
            }
        }
        std::lock_guard morph_guard(m_morph_lock);
        for (size_t i = 0; i < out.size(); ++i)
        {
            const auto it = m_morph_books.find(keys[i]);
            if (it != m_morph_books.end())
            {
                out[i].morphs = it->second;
            }
        }
        return out;
    }

    std::vector<std::string> Registry::take_forgotten_identities()
    {
        std::lock_guard guard(m_lock);
        std::vector<std::string> out;
        out.swap(m_forgotten);
        return out;
    }

    void Registry::set_remembered(std::vector<std::string> identities)
    {
        std::lock_guard guard(m_lock);
        m_remembered = std::move(identities);
    }

    std::vector<std::string> Registry::remembered() const
    {
        std::lock_guard guard(m_lock);
        return m_remembered;
    }

    EditBook Registry::edits_of(const std::string& key) const
    {
        std::lock_guard guard(m_lock);
        const auto it = m_actors.find(key);
        return it == m_actors.end() ? EditBook{} : it->second.book;
    }

    void Registry::replace_edits_of(const std::string& key, EditBook book)
    {
        std::lock_guard guard(m_lock);
        Actor& a = actor_locked(key);
        a.book = std::move(book);
        bump_edits(a);
    }

    MorphBook Registry::morphs_of(const std::string& key) const
    {
        std::lock_guard guard(m_morph_lock);
        const auto it = m_morph_books.find(key);
        return it == m_morph_books.end() ? MorphBook{} : it->second;
    }

    void Registry::replace_morphs_of(const std::string& key, MorphBook book)
    {
        for (auto& [k, e] : book)
        {
            e.weight = clamp_morph(e.weight);
        }
        std::lock_guard guard(m_morph_lock);
        if (book.empty())
        {
            m_morph_books.erase(key);
        }
        else
        {
            m_morph_books[key] = std::move(book);
        }
        m_morph_revision.fetch_add(1, std::memory_order_acq_rel);
    }

    std::map<std::string, MorphBook> Registry::all_morphs(uint64_t* revision) const
    {
        std::lock_guard guard(m_morph_lock);
        if (revision)
        {
            *revision = m_morph_revision.load(std::memory_order_acquire);
        }
        return m_morph_books;
    }

    EditBook Registry::edits(uint64_t* revision) const
    {
        std::lock_guard guard(m_lock);
        if (revision)
        {
            *revision = m_edit_revision.load(std::memory_order_acquire);
        }
        const auto it = m_actors.find(m_active);
        return it == m_actors.end() ? EditBook{} : it->second.book;
    }

    void Registry::set_bone(const std::string& bone, const BoneEdit& scale)
    {
        if (bone.empty())
        {
            return;
        }
        BoneEdit s = scale;
        s.clamp();
        std::lock_guard guard(m_lock);
        Actor& a = active_locked();
        EditEntry& entry = a.book[fold_case(bone)];
        entry.bone = bone;
        entry.edit = s;
        bump_edits(a);
    }

    void Registry::clear_bone(const std::string& bone)
    {
        std::lock_guard guard(m_lock);
        Actor& a = active_locked();
        if (a.book.erase(fold_case(bone)) > 0)
        {
            bump_edits(a);
        }
    }

    void Registry::replace_edits(EditBook book)
    {
        std::lock_guard guard(m_lock);
        Actor& a = active_locked();
        a.book = std::move(book);
        bump_edits(a);
    }

    void Registry::clear_edits()
    {
        std::lock_guard guard(m_lock);
        Actor& a = active_locked();
        a.book.clear();
        bump_edits(a);
    }

    // ---- locomotion state layer -------------------------------------------------
    LocoBookMap Registry::loco_all(uint64_t* revision) const
    {
        std::lock_guard guard(m_lock);
        if (revision)
        {
            *revision = m_loco_revision.load(std::memory_order_acquire);
        }
        return m_loco_books;
    }

    std::vector<std::string> Registry::loco_contexts(uint64_t* revision) const
    {
        std::lock_guard guard(m_lock);
        if (revision)
        {
            *revision = m_loco_revision.load(std::memory_order_acquire);
        }
        return m_loco_known;
    }

    void Registry::remember_loco_context(const std::string& context)
    {
        if (context.empty())
        {
            return;
        }
        std::lock_guard guard(m_lock);
        if (std::find(m_loco_known.begin(), m_loco_known.end(), context) == m_loco_known.end())
        {
            m_loco_known.push_back(context);
            m_loco_revision.fetch_add(1, std::memory_order_acq_rel);
        }
    }

    void Registry::set_loco_bone(const std::string& context, int state, const std::string& bone, const BoneEdit& edit)
    {
        if (context.empty() || bone.empty() || state < 0 || state >= kLocoStateCount)
        {
            return;
        }
        BoneEdit value = edit;
        value.clamp();
        std::lock_guard guard(m_lock);
        EditEntry& entry = m_loco_books[context][static_cast<size_t>(state)][fold_case(bone)];
        entry.bone = bone;
        entry.edit = value;
        if (std::find(m_loco_known.begin(), m_loco_known.end(), context) == m_loco_known.end())
        {
            m_loco_known.push_back(context);
        }
        m_loco_revision.fetch_add(1, std::memory_order_acq_rel);
    }

    void Registry::clear_loco_bone(const std::string& context, int state, const std::string& bone)
    {
        if (context.empty() || state < 0 || state >= kLocoStateCount)
        {
            return;
        }
        std::lock_guard guard(m_lock);
        const auto it = m_loco_books.find(context);
        if (it != m_loco_books.end() && it->second[static_cast<size_t>(state)].erase(fold_case(bone)) > 0)
        {
            m_loco_revision.fetch_add(1, std::memory_order_acq_rel);
        }
    }

    void Registry::replace_loco_book(const std::string& context, int state, EditBook book)
    {
        if (context.empty() || state < 0 || state >= kLocoStateCount)
        {
            return;
        }
        std::lock_guard guard(m_lock);
        m_loco_books[context][static_cast<size_t>(state)] = std::move(book);
        if (std::find(m_loco_known.begin(), m_loco_known.end(), context) == m_loco_known.end())
        {
            m_loco_known.push_back(context);
        }
        m_loco_revision.fetch_add(1, std::memory_order_acq_rel);
    }

    void Registry::replace_loco_all(LocoBookMap books)
    {
        std::lock_guard guard(m_lock);
        m_loco_books = std::move(books);
        for (const auto& [context, states] : m_loco_books)
        {
            if (std::find(m_loco_known.begin(), m_loco_known.end(), context) == m_loco_known.end())
            {
                m_loco_known.push_back(context);
            }
        }
        m_loco_revision.fetch_add(1, std::memory_order_acq_rel);
    }

    void Registry::set_loco_live(const std::string& context, int state, bool layer_on)
    {
        std::lock_guard guard(m_lock);
        m_loco_live_context = context.empty() ? std::string("unarmed") : context;
        m_loco_live_state = (state >= -1 && state < kLocoStateCount) ? state : -1;
        m_loco_layer_on = layer_on;
    }

    std::string Registry::loco_live_context() const
    {
        std::lock_guard guard(m_lock);
        return m_loco_live_context;
    }

    int Registry::loco_live_state() const
    {
        std::lock_guard guard(m_lock);
        return m_loco_live_state;
    }

    bool Registry::loco_layer_on() const
    {
        std::lock_guard guard(m_lock);
        return m_loco_layer_on;
    }

    void Registry::set_loco_preview(const std::string& context, int state)
    {
        std::lock_guard guard(m_lock);
        m_loco_preview_context = context;
        m_loco_preview_state = (state >= 0 && state < kLocoStateCount) ? state : -1;
    }

    // Advances the cross-fade and works out the layer that should be applied this frame.
    // Called from the pose hook (m_lock held), at most once per millisecond.
    void Registry::update_loco_layer_locked()
    {
        const auto now = std::chrono::steady_clock::now();
        const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        if (now_ms == m_loco_layer_ms)
        {
            return;
        }
        m_loco_layer_ms = now_ms;

        const bool preview_on = !m_loco_preview_context.empty() && m_loco_preview_state >= 0;
        const std::string& context = preview_on ? m_loco_preview_context : m_loco_live_context;
        const int state = preview_on ? m_loco_preview_state : m_loco_live_state;
        const bool on = preview_on ? true : m_loco_layer_on;
        const uint64_t rev = m_loco_revision.load(std::memory_order_acquire);
        const bool valid = !context.empty() && state >= 0 && state < kLocoStateCount;

        auto book_of = [this](bool show, const std::string& ctx, int st) -> const EditBook& {
            static const EditBook empty;
            if (!show || ctx.empty() || st < 0 || st >= kLocoStateCount)
            {
                return empty;
            }
            const auto it = m_loco_books.find(ctx);
            return it == m_loco_books.end() ? empty : it->second[static_cast<size_t>(st)];
        };

        const bool target_on = on && valid;
        const std::string target_context = valid ? context : std::string();
        const int target_state = valid ? state : -1;
        const bool mode_changed = target_on != m_loco_target_on || target_state != m_loco_target_state ||
                                  target_context != m_loco_target_context;
        const bool values_changed = rev != m_loco_target_revision;

        if (mode_changed || values_changed)
        {
            m_loco_target_revision = rev;
            m_loco_target_on = target_on;
            m_loco_target_state = target_state;
            m_loco_target_context = target_context;
            const int blend = m_loco_blend_ms.load(std::memory_order_relaxed);
            if (mode_changed && blend > 0)
            {
                m_loco_fade_from = m_loco_layer;
                m_loco_fade_start = now;
                m_loco_fade_ms = blend;
                m_loco_fade_active = true;
            }
            else
            {
                // Edits inside the current context/state (slider drags) and blend = 0 apply at once.
                m_loco_fade_active = false;
                m_loco_layer = book_of(target_on, target_context, target_state);
                ++m_loco_layer_serial;
            }
        }

        if (m_loco_fade_active)
        {
            const EditBook& target = book_of(m_loco_target_on, m_loco_target_context, m_loco_target_state);
            const double elapsed = std::chrono::duration<double, std::milli>(now - m_loco_fade_start).count();
            const double raw = m_loco_fade_ms > 0 ? elapsed / static_cast<double>(m_loco_fade_ms) : 1.0;
            if (raw >= 1.0)
            {
                m_loco_layer = target;
                m_loco_fade_active = false;
            }
            else
            {
                const double t = raw <= 0.0 ? 0.0 : raw * raw * (3.0 - 2.0 * raw); // smoothstep
                m_loco_layer = blend_books(m_loco_fade_from, target, t);
            }
            ++m_loco_layer_serial;
        }
    }

    // ---- 时间轴动画 ---------------------------------------------------------
    std::vector<Animation> Registry::animations() const
    {
        std::lock_guard guard(m_lock);
        return m_anims;
    }

    void Registry::set_animations(std::vector<Animation> animations)
    {
        std::lock_guard guard(m_lock);
        m_anims = std::move(animations);
        // 保留已有播放进度：只把运行时数组对齐到新的轨道数量。
        m_anim_runtimes.resize(m_anims.size());
        for (size_t i = 0; i < m_anims.size(); ++i)
        {
            m_anim_runtimes[i].tracks.resize(m_anims[i].tracks.size());
        }
        m_anim_revision.fetch_add(1, std::memory_order_acq_rel);
    }

    void Registry::restart_animations()
    {
        std::lock_guard guard(m_lock);
        for (AnimRuntime& runtime : m_anim_runtimes)
        {
            runtime = AnimRuntime{};
        }
        m_anim_layer.clear();
        ++m_anim_layer_serial;
        m_anim_last_tick = {};
    }

    std::vector<std::vector<Registry::AnimLiveTrack>> Registry::anim_live()
    {
        std::lock_guard guard(m_lock);
        std::vector<std::vector<AnimLiveTrack>> out(m_anims.size());
        for (size_t a = 0; a < m_anims.size(); ++a)
        {
            const Animation& animation = m_anims[a];
            AnimRuntime& runtime = m_anim_runtimes[a];
            runtime.tracks.resize(animation.tracks.size());
            out[a].resize(animation.tracks.size());
            for (size_t t = 0; t < animation.tracks.size(); ++t)
            {
                AnimLiveTrack live;
                anim_track_progress(animation.tracks[t], runtime.tracks[t], live.node, live.progress);
                live.finished = runtime.tracks[t].finished;
                out[a][t] = live;
            }
        }
        return out;
    }

    // 推进动画，把结果收成「骨骼 -> 叠加值」的一本书。同一帧里多个网格都会走到这里，
    // 所以用时间戳保证每帧只真正推进一次（dt 小于 0.5ms 视为同一帧）。
    void Registry::update_anim_layer_locked()
    {
        const auto now = std::chrono::steady_clock::now();
        const bool first_tick = m_anim_last_tick == std::chrono::steady_clock::time_point{};
        double dt = first_tick ? 0.0 : std::chrono::duration<double>(now - m_anim_last_tick).count();
        if (dt > 0.25)
        {
            dt = 0.25; // 卡顿/暂停之后不要把整段动画一下跳过
        }
        const bool playing = m_anim_playing.load(std::memory_order_relaxed);
        const bool advance = playing && dt >= 0.0005;
        if (advance || first_tick)
        {
            m_anim_last_tick = now;
        }

        EditBook layer;
        for (size_t a = 0; a < m_anims.size(); ++a)
        {
            Animation& animation = m_anims[a];
            if (!animation.enabled)
            {
                continue;
            }
            AnimRuntime& runtime = m_anim_runtimes[a];
            runtime.tracks.resize(animation.tracks.size());
            for (size_t t = 0; t < animation.tracks.size(); ++t)
            {
                AnimTrack& track = animation.tracks[t];
                if (!track.enabled || track.bones.empty())
                {
                    continue;
                }
                const BoneEdit value = anim_track_advance(track, runtime.tracks[t], advance ? dt : 0.0);
                if (value.is_neutral())
                {
                    continue;
                }
                BoneEdit edit = value;
                edit.spread = track.spread;
                for (const std::string& bone : track.bones)
                {
                    const std::string key = fold_case(bone);
                    if (key.empty())
                    {
                        continue;
                    }
                    const auto it = layer.find(key);
                    if (it == layer.end())
                    {
                        EditEntry entry;
                        entry.bone = bone;
                        entry.edit = edit;
                        layer.emplace(key, std::move(entry));
                        continue;
                    }
                    for (int i = 0; i < 3; ++i)
                    {
                        it->second.edit.axis[i] *= edit.axis[i];
                        it->second.edit.turn[i] += edit.turn[i];
                        it->second.edit.shift[i] += edit.shift[i];
                    }
                    it->second.edit.clamp();
                }
            }
        }

        const bool had_layer = !m_anim_layer.empty();
        m_anim_layer = std::move(layer);
        if (advance || had_layer || !m_anim_layer.empty())
        {
            // 有动画时每帧都重新规划一次（只涉及动画里的几根骨骼），这样停止/恢复也能立刻反映。
            ++m_anim_layer_serial;
        }
    }

    void Registry::set_enabled(bool on)
    {
        m_enabled.store(on, std::memory_order_relaxed);
        m_morph_revision.fetch_add(1, std::memory_order_acq_rel);
    }

    MorphBook Registry::morphs(uint64_t* revision) const
    {
        std::lock_guard guard(m_morph_lock);
        if (revision)
        {
            *revision = m_morph_revision.load(std::memory_order_acquire);
        }
        const auto it = m_morph_books.find(m_morph_active);
        return it == m_morph_books.end() ? MorphBook{} : it->second;
    }

    void Registry::set_morph(const std::string& morph, double weight)
    {
        if (morph.empty())
        {
            return;
        }
        std::lock_guard guard(m_morph_lock);
        MorphEntry& e = m_morph_books[m_morph_active][fold_case(morph)];
        e.name = morph;
        e.weight = clamp_morph(weight);
        m_morph_revision.fetch_add(1, std::memory_order_acq_rel);
    }

    void Registry::clear_morph(const std::string& morph)
    {
        std::lock_guard guard(m_morph_lock);
        auto it = m_morph_books.find(m_morph_active);
        if (it != m_morph_books.end() && it->second.erase(fold_case(morph)) > 0)
        {
            if (it->second.empty())
            {
                m_morph_books.erase(it);
            }
            m_morph_revision.fetch_add(1, std::memory_order_acq_rel);
        }
    }

    void Registry::replace_morphs(MorphBook book)
    {
        for (auto& [key, e] : book)
        {
            e.weight = clamp_morph(e.weight);
        }
        std::lock_guard guard(m_morph_lock);
        if (book.empty())
        {
            m_morph_books.erase(m_morph_active);
        }
        else
        {
            m_morph_books[m_morph_active] = std::move(book);
        }
        m_morph_revision.fetch_add(1, std::memory_order_acq_rel);
    }

    void Registry::clear_morphs()
    {
        std::lock_guard guard(m_morph_lock);
        m_morph_books.erase(m_morph_active);
        m_morph_revision.fetch_add(1, std::memory_order_acq_rel);
    }

    void Registry::set_morph_names(std::vector<std::string> names)
    {
        std::lock_guard guard(m_morph_lock);
        if (names != m_morph_names)
        {
            m_morph_names = std::move(names);
            m_morph_names_revision.fetch_add(1, std::memory_order_acq_rel);
        }
    }

    std::vector<std::string> Registry::morph_names(uint64_t* revision) const
    {
        std::lock_guard guard(m_morph_lock);
        if (revision)
        {
            *revision = m_morph_names_revision.load(std::memory_order_acquire);
        }
        return m_morph_names;
    }

    void Registry::set_animated_morphs(std::set<std::string> folded)
    {
        std::lock_guard guard(m_morph_lock);
        m_animated_morphs = std::move(folded);
    }

    std::set<std::string> Registry::animated_morphs() const
    {
        std::lock_guard guard(m_morph_lock);
        return m_animated_morphs;
    }

    SkeletonView Registry::skeleton(uint64_t* revision) const
    {
        std::lock_guard guard(m_lock);
        if (revision)
        {
            *revision = m_skeleton_revision.load(std::memory_order_acquire);
        }
        return m_view;
    }

    std::vector<RigSummary> Registry::rigs() const
    {
        std::lock_guard guard(m_lock);
        std::vector<RigSummary> out;
        out.reserve(m_rigs.size());
        for (const auto& rig : m_rigs)
        {
            out.push_back({rig->label, rig->owner, rig->sculptor.bone_count(), rig->frames, rig->stale, rig->primary, rig->actor_key, rig->actor_key == m_active});
        }
        return out;
    }

    MirrorTable Registry::mirror_table() const
    {
        std::lock_guard guard(m_lock);
        return m_mirror;
    }

    std::string Registry::mirror_source() const
    {
        std::lock_guard guard(m_lock);
        return m_mirror_source;
    }

    BoneEdit Registry::mirror_edit(const std::string& bone, const BoneEdit& edit) const
    {
        std::lock_guard guard(m_lock);
        return mirror_with(m_mirror, bone, edit);
    }
} // namespace uuepbs
