#include "panel_view.hpp"
#include "i18n.hpp"

#include "../core/rig_names.hpp"
#include "skin.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

namespace uuepbs::ui
{
    namespace
    {
        // Colours come from the active skin (the built-in look when there is none).
        const ImVec4& accent() { return skin().color(Role::Accent); }
        const ImVec4& accent_dim() { return skin().color(Role::AccentDim); }
        const ImVec4& good() { return skin().color(Role::Good); }
        const ImVec4& warn() { return skin().color(Role::Warn); }
        const ImVec4& muted() { return skin().color(Role::Muted); }

        // Log scale from 0.1x to 10x puts 1.0x exactly in the middle of the slider.
        constexpr float kScaleMin = 0.10f;
        constexpr float kScaleMax = 10.0f;
        // Morph sliders show 0..1; Ctrl+click can type anything in kMorphMin..kMorphMax.
        constexpr float kMorphSliderMin = 0.0f;
        constexpr float kMorphSliderMax = 1.0f;

        bool contains_folded(const std::string& haystack, const std::string& folded_needle)
        {
            return folded_needle.empty() || fold_case(haystack).find(folded_needle) != std::string::npos;
        }
    } // namespace

    // Ctrl + mouse wheel over the slider just drawn nudges it by `step`
    // (Ctrl + Shift: by `fine`). The plain wheel keeps scrolling the window.
    static bool wheel_nudge(float& v, float step, float fine, float lo, float hi)
    {
        const ImGuiIO& io = ImGui::GetIO();
        if (!io.KeyCtrl)
        {
            return false;
        }
        ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
        if (!ImGui::IsItemHovered() || io.MouseWheel == 0.0f)
        {
            return false;
        }
        const float delta = (io.KeyShift ? fine : step) * (io.MouseWheel > 0.0f ? 1.0f : -1.0f);
        v = std::min(hi, std::max(lo, v + delta));
        return true;
    }

    void PanelView::configure(const PresetShelf* shelf, PanelHost* host, TargetSource* targets, std::string version, bool topmost)
    {
        m_shelf = shelf;
        m_host = host;
        m_targets = targets;
        m_version = std::move(version);
        m_topmost = topmost;
    }

    void PanelView::set_message(const std::string& text)
    {
        std::lock_guard guard(m_message_lock);
        m_message = text;
        m_message_time = Clock::now();
    }

    void PanelView::apply_theme(float dpi)
    {
        skin().apply_style(dpi);
    }

    void PanelView::sync()
    {
        Registry& reg = Registry::instance();
        if (reg.skeleton_revision() != m_skeleton_revision)
        {
            m_skeleton = reg.skeleton(&m_skeleton_revision);
            m_bone_keys.clear();
            for (const auto& n : m_skeleton.names)
            {
                m_bone_keys.insert(fold_case(n));
            }
            m_body_revision = 0; // re-resolve below
        }
        const uint64_t body_rev = BodyMapSource::instance().revision();
        if (body_rev != m_body_revision)
        {
            BoneDictionary dict;
            std::vector<UserGroup> user;
            BodyMapSource::instance().get(dict, user, m_body_status);
            m_body = resolve_body_groups(m_skeleton.names, m_skeleton.parents, dict, user);
            m_bone_group.clear();
            for (const GroupBones& gb : m_body.groups)
            {
                for (const std::string& b : gb.bones)
                {
                    m_bone_group[fold_case(b)] = gb.group.title;
                }
            }
            m_body_revision = body_rev;
        }
        if (reg.edit_revision() != m_book_revision)
        {
            m_book = reg.edits(&m_book_revision);
        }
        if (reg.loco_revision() != m_loco_revision)
        {
            m_loco_ctx = reg.loco_all();
            m_loco_known = reg.loco_contexts();
            m_loco_revision = reg.loco_revision();
        }
        m_loco_live = reg.loco_live_state();
        m_loco_live_context = reg.loco_live_context();
        m_loco_layer_on = reg.loco_layer_on();

        // Morph targets: names from the meshes, minus the profile's ExcludeMorphs.
        const uint64_t profile_rev = MorphProfileSource::instance().revision();
        if (reg.morph_names_revision() != m_morph_names_revision || profile_rev != m_morph_profile_revision)
        {
            MorphProfileSource::instance().get(m_morph_profile, m_morph_status);
            m_morph_profile_revision = profile_rev;
            const std::vector<std::string> all = reg.morph_names(&m_morph_names_revision);
            m_morph_names.clear();
            m_morph_keys.clear();
            for (const std::string& m : all)
            {
                if (!m_morph_profile.excluded(m))
                {
                    m_morph_names.push_back(m);
                    m_morph_keys.insert(fold_case(m));
                }
            }
            m_morphs_hidden = all.size() - m_morph_names.size();
            m_morph_groups = resolve_morph_groups(m_morph_names, m_morph_profile);
        }
        if (reg.morph_revision() != m_morph_revision)
        {
            m_morphs = reg.morphs(&m_morph_revision);
        }

        const auto now = Clock::now();
        if (now - m_rigs_time > std::chrono::milliseconds(500))
        {
            m_rigs = reg.rigs();
            m_edited = reg.edited_actors();
            m_remembered = reg.remembered();
            m_rigs_time = now;
            m_mirror_text = reg.mirror_source();
            m_animated = reg.animated_morphs();
        }
        if (m_targets && now - m_target_time > std::chrono::milliseconds(400))
        {
            if (!m_target_combo_open || now - m_target_combo_since < std::chrono::milliseconds(2500))
            {
                m_target_list = m_targets->targets();
            }
            m_target = m_targets->current_target();
            m_target_time = now;
        }
    }

    EditBook& PanelView::active_book()
    {
        if (!m_loco_edit)
        {
            return m_book;
        }
        return m_loco_ctx[m_loco_context][static_cast<size_t>(m_loco_state)];
    }

    const EditBook& PanelView::active_book() const
    {
        if (!m_loco_edit)
        {
            return m_book;
        }
        const auto it = m_loco_ctx.find(m_loco_context);
        if (it == m_loco_ctx.end())
        {
            return m_loco_fallback;
        }
        return it->second[static_cast<size_t>(m_loco_state)];
    }

    bool PanelView::has_bone(const std::string& bone) const
    {
        return m_bone_keys.count(fold_case(bone)) > 0;
    }

    bool PanelView::has_morph(const std::string& morph) const
    {
        return m_morph_keys.count(fold_case(morph)) > 0;
    }

    Spread PanelView::spread_of(const std::string& bone, Spread fallback) const
    {
        const std::string key = fold_case(bone);
        const auto it = active_book().find(key);
        if (it != active_book().end())
        {
            return it->second.edit.spread;
        }
        const auto hint = m_spread_hint.find(key);
        return hint != m_spread_hint.end() ? hint->second : fallback;
    }

    BoneEdit PanelView::scale_of(const std::string& bone, Spread fallback) const
    {
        const auto it = active_book().find(fold_case(bone));
        if (it != active_book().end())
        {
            return it->second.edit;
        }
        BoneEdit s;
        s.spread = spread_of(bone, fallback);
        return s;
    }

    void PanelView::write(const std::string& bone, BoneEdit s)
    {
        Registry& reg = Registry::instance();
        s.clamp();
        const std::string key = fold_case(bone);
        m_spread_hint[key] = s.spread;

        if (m_loco_edit && m_loco_state >= 0 && m_loco_state < uuepbs::kLocoStateCount && !m_loco_context.empty())
        {
            EditBook& book = m_loco_ctx[m_loco_context][static_cast<size_t>(m_loco_state)];
            if (s.is_neutral())
            {
                reg.clear_loco_bone(m_loco_context, m_loco_state, bone);
                book.erase(key);
            }
            else
            {
                reg.set_loco_bone(m_loco_context, m_loco_state, bone, s);
                book[key] = EditEntry{bone, s};
            }
            m_loco_revision = reg.loco_revision();
            return;
        }

        if (s.is_neutral())
        {
            reg.clear_bone(bone);
            m_book.erase(key);
        }
        else
        {
            reg.set_bone(bone, s);
            m_book[key] = EditEntry{bone, s};
        }
        m_book_revision = reg.edit_revision();
    }

    // Simplified Panel sliders only change scale; each bone keeps its own rotation/move.
    void PanelView::write_group(const GroupBones& group, const double axis[3])
    {
        for (const std::string& bone : group.bones)
        {
            if (!has_bone(bone))
            {
                continue;
            }
            BoneEdit s = scale_of(bone, group.group.spread);
            s.axis[0] = axis[0];
            s.axis[1] = axis[1];
            s.axis[2] = axis[2];
            s.spread = spread_of(bone, group.group.spread);
            write(bone, s);
        }
    }

    std::optional<double> PanelView::morph_of(const std::string& morph) const
    {
        const auto it = m_morphs.find(fold_case(morph));
        if (it == m_morphs.end())
        {
            return std::nullopt;
        }
        return it->second.weight;
    }

    // nullopt gives the morph back to the game.
    void PanelView::write_morph(const std::string& morph, std::optional<double> weight)
    {
        Registry& reg = Registry::instance();
        const std::string key = fold_case(morph);
        if (weight)
        {
            const double w = clamp_morph(*weight);
            reg.set_morph(morph, w);
            m_morphs[key] = MorphEntry{morph, w};
        }
        else
        {
            reg.clear_morph(morph);
            m_morphs.erase(key);
        }
        m_morph_revision = reg.morph_revision();
    }

    bool PanelView::size_slider(const char* id, BoneEdit& s, float lo, float hi)
    {
        const double avg = (s.axis[0] + s.axis[1] + s.axis[2]) / 3.0;
        float v = static_cast<float>(avg);
        const bool dragged = skin().slider_float(id, &v, lo, hi, "%.3fx", ImGuiSliderFlags_Logarithmic);
        const bool nudged = wheel_nudge(v, 0.01f, 0.001f, lo, hi);
        if (!dragged && !nudged)
        {
            return false;
        }
        const bool uniform = std::fabs(s.axis[0] - s.axis[1]) < 1e-6 && std::fabs(s.axis[1] - s.axis[2]) < 1e-6;
        if (uniform || avg <= 1e-6)
        {
            s.axis[0] = s.axis[1] = s.axis[2] = v;
        }
        else
        {
            const double k = v / avg;
            for (double& a : s.axis)
            {
                a *= k;
            }
        }
        return true;
    }

    bool PanelView::axis_sliders(BoneEdit& s)
    {
        const char* names[3] = {tr("Length (X)"), tr("Width (Y)"), tr("Depth (Z)")};
        bool changed = false;
        for (int a = 0; a < 3; ++a)
        {
            float v = static_cast<float>(s.axis[a]);
            ImGui::PushID(a);
            ImGui::SetNextItemWidth(-ImGui::CalcTextSize(tr("Length (X)")).x - ImGui::GetStyle().ItemInnerSpacing.x * 2);
            if (skin().slider_float(names[a], &v, kScaleMin, kScaleMax, "%.3fx", ImGuiSliderFlags_Logarithmic))
            {
                s.axis[a] = v;
                changed = true;
            }
            if (wheel_nudge(v, 0.01f, 0.001f, kScaleMin, kScaleMax))
            {
                s.axis[a] = v;
                changed = true;
            }
            ImGui::PopID();
        }
        return changed;
    }

    // Shared layout for a labelled three-row slider block.
    static bool triple_sliders(const char* const labels[3], double (&v)[3], float lo, float hi, const char* format, float step, float fine)
    {
        bool changed = false;
        for (int a = 0; a < 3; ++a)
        {
            float f = static_cast<float>(v[a]);
            ImGui::PushID(labels[a]);
            ImGui::SetNextItemWidth(-ImGui::CalcTextSize(tr("Length (X)")).x - ImGui::GetStyle().ItemInnerSpacing.x * 2);
            const bool dragged = skin().slider_float(labels[a], &f, lo, hi, format);
            if (dragged || wheel_nudge(f, step, fine, -BoneEdit::kMaxShift * 10.0f, BoneEdit::kMaxShift * 10.0f))
            {
                v[a] = f;
                changed = true;
            }
            ImGui::PopID();
        }
        return changed;
    }

    bool PanelView::turn_sliders(BoneEdit& s)
    {
        const char* const labels[3] = {tr("Roll (X)"), tr("Pitch (Y)"), tr("Yaw (Z)")};
        return triple_sliders(labels, s.turn, -180.0f, 180.0f, "%.1f\xC2\xB0", 1.0f, 0.1f);
    }

    bool PanelView::shift_sliders(BoneEdit& s)
    {
        const char* const labels[3] = {tr("Along (X)"), tr("Side (Y)"), tr("Up (Z)")};
        return triple_sliders(labels, s.shift, -20.0f, 20.0f, tr("%.2f cm"), 0.1f, 0.01f);
    }

    bool PanelView::morph_slider(const char* id, float& v)
    {
        const bool dragged = skin().slider_float(id, &v, kMorphSliderMin, kMorphSliderMax, "%.2f");
        const bool nudged = wheel_nudge(v, 0.05f, 0.01f, static_cast<float>(kMorphMin), static_cast<float>(kMorphMax));
        v = static_cast<float>(clamp_morph(v)); // typed values (Ctrl+click) may leave 0..1
        return dragged || nudged;
    }

    void PanelView::tooltip(const char* text)
    {
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && text && *text)
        {
            ImGui::SetTooltip(tr("%s"), text);
        }
    }

    void PanelView::draw()
    {
        sync();
        Skin& sk = skin();
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        const ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
        if (sk.font())
        {
            ImGui::PushFont(sk.font(), 0.0f);
        }
        ImGui::Begin("##uuepbs", nullptr, flags);
        sk.draw_window_background();

        draw_header();
        m_loco_seen = false;
        if (ImGui::BeginTabBar("##tabs"))
        {
            auto tab_flags = [this](Tab tab) {
                return m_focus_tab == static_cast<int>(tab) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            };
            if (sk.begin_tab_item(tr("Simplified Panel###simplified"), "simplified", tab_flags(Tab::Simplified)))
            {
                draw_simplified();
                ImGui::EndTabItem();
            }
            if (sk.begin_tab_item(tr("Detailed Panel###detailed"), "detailed", tab_flags(Tab::Detailed)))
            {
                draw_detailed();
                ImGui::EndTabItem();
            }
            if (sk.begin_tab_item(tr("Locomotion###locomotion"), {}, tab_flags(Tab::Locomotion)))
            {
                draw_locomotion();
                ImGui::EndTabItem();
            }
            if (sk.begin_tab_item(tr("Animation###animation"), {}, tab_flags(Tab::Animation)))
            {
                draw_animations();
                ImGui::EndTabItem();
            }
            if (sk.begin_tab_item(tr("Presets###presets"), "presets", tab_flags(Tab::Presets)))
            {
                draw_presets();
                ImGui::EndTabItem();
            }
            if (sk.begin_tab_item(tr("Status###status"), "status", tab_flags(Tab::Status)))
            {
                draw_status();
                ImGui::EndTabItem();
            }
            m_focus_tab = -1;
            ImGui::EndTabBar();
        }
        if (!m_loco_seen)
        {
            // The Locomotion tab is not showing: a force-preview must never leak into normal play.
            Registry::instance().set_loco_preview(std::string(), -1);
        }
        ImGui::End();
        if (sk.font())
        {
            ImGui::PopFont();
        }
    }

    void PanelView::draw_header()
    {
        Skin& sk = skin();
        const float base = ImGui::GetStyle().FontSizeBase;
        sk.draw_header_strip(std::max(ImGui::GetFontSize() * 1.18f, ImGui::GetFrameHeight()));
        if (!sk.logo(ImGui::GetFontSize()))
        {
            ImGui::PushFont(nullptr, base * 1.18f);
            ImGui::PushStyleColor(ImGuiCol_Text, sk.color(Role::Title));
            ImGui::TextUnformatted(tr("UUEPBS"));
            ImGui::PopStyleColor();
            ImGui::PopFont();
        }
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(muted(), tr("%s"), m_version.c_str());
        ImGui::SameLine();
        ImGui::PushFont(nullptr, base * 0.85f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImVec4(muted().x, muted().y, muted().z, 0.70f), tr("by XTGMods"));
        ImGui::PopFont();

        bool on = Registry::instance().enabled();
        const float box = ImGui::GetFrameHeight() + ImGui::CalcTextSize(tr("Enabled")).x + ImGui::GetStyle().ItemInnerSpacing.x;
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - box)); // right-align
        if (sk.checkbox(tr("Enabled"), &on))
        {
            Registry::instance().set_enabled(on);
        }

        draw_target_picker();

        size_t live = 0;
        for (const auto& r : m_rigs)
        {
            live += (!r.stale && r.frames > 0) ? 1 : 0;
        }
        if (m_skeleton.names.empty())
        {
            ImGui::TextColored(warn(), tr("No character found yet. Pick one above or use Status > Rescan."));
        }
        else if (!m_host->hook_active())
        {
            ImGui::TextColored(warn(), tr("Found %s - waiting for the pose hook (see Status)"), m_skeleton.owner.empty() ? m_skeleton.label.c_str() : m_skeleton.owner.c_str());
        }
        else if (live == 0)
        {
            ImGui::TextColored(warn(), tr("Tracking %zu mesh%s - waiting for an animated frame"), m_rigs.size(), m_rigs.size() == 1 ? "" : "es");
        }
        else
        {
            ImGui::TextColored(good(), tr("Live on %zu mesh%s"), live, live == 1 ? "" : "es");
            ImGui::SameLine();
            if (m_morph_names.empty())
            {
                ImGui::TextColored(muted(), tr("- %s, %zu bones"), m_skeleton.label.c_str(), m_skeleton.names.size());
            }
            else
            {
                ImGui::TextColored(muted(), tr("- %s, %zu bones, %zu morphs"), m_skeleton.label.c_str(), m_skeleton.names.size(), m_morph_names.size());
            }
        }

        std::string message;
        {
            std::lock_guard guard(m_message_lock);
            if (Clock::now() - m_message_time < std::chrono::seconds(6))
            {
                message = m_message;
            }
        }
        if (!message.empty())
        {
            ImGui::TextColored(accent(), tr("%s"), message.c_str());
        }
        ImGui::Spacing();
    }

    void PanelView::draw_target_picker()
    {
        if (!m_targets)
        {
            return;
        }
        Skin& sk = skin();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(tr("Character"));
        ImGui::SameLine();
        const float refresh_w = sk.button_width(tr("Refresh"), "refresh");
        ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x - refresh_w - ImGui::GetStyle().ItemSpacing.x));
        const std::string shown = m_target.label.empty() ? std::string(tr("(searching...)")) : m_target.label;
        const bool was_open = m_target_combo_open;
        m_target_combo_open = ImGui::BeginCombo("##target", shown.c_str(), ImGuiComboFlags_HeightLarge);
        if (m_target_combo_open && !was_open)
        {
            // Just opened: ask for the list sorted by the current distances (cheap; Refresh searches the world).
            m_target_combo_since = Clock::now();
            m_targets->relist_targets();
        }
        if (m_target_combo_open)
        {
            const std::string auto_label = std::string(tr("Automatic (player / game default)")) + (is_edited(kPlayerActor) ? tr("   - edited") : "");
            if (ImGui::Selectable(auto_label.c_str(), m_target.id == "auto"))
            {
                m_targets->pick_target("auto");
                set_message(tr("Switching to the default character"));
            }
            if (!m_target_list.empty())
            {
                ImGui::Separator();
            }
            for (const TargetChoice& t : m_target_list)
            {
                ImGui::PushID(t.id.c_str());
                const bool edited = is_edited(t.key);
                if (edited)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, accent());
                }
                const std::string label = t.label + (edited ? tr("   - edited") : "");
                if (ImGui::Selectable(label.c_str(), t.id == m_target.id))
                {
                    m_targets->pick_target(t.id);
                    set_message(tr("Switching to " )+ t.label);
                }
                if (edited)
                {
                    ImGui::PopStyleColor();
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        tooltip(tr("Whose body the sliders edit. The list shows characters currently loaded in the level, nearest first,\n"
                "with their distance from the player. Characters you have edited are always listed.\n"
                "Characters you have edited keep their sliders while you edit someone else (marked \"edited\").\n"
                "Characters you never edited are left alone."));
        ImGui::SameLine();
        if (sk.button(tr("Refresh"), "refresh"))
        {
            m_targets->refresh_targets();
            m_targets->request_rescan();
            set_message(tr("Looking for characters..."));
        }
        {
            const std::string key = m_targets->refresh_hotkey_text();
            const std::string tip = std::string(tr("Rescan the character and its meshes, relist the characters and retry the pose hook.")) +
                                    (key.empty() ? std::string() : std::string(tr("\nHotkey: ")) + key + tr(" (works in game too)."));
            tooltip(tip.c_str());
        }

        // Other characters that keep their sliders while this one is edited.
        std::string others;
        size_t n_others = 0;
        for (const ActorSummary& a : m_edited)
        {
            if (!a.active)
            {
                others += (others.empty() ? "" : ", ") + a.label;
                ++n_others;
            }
        }
        if (n_others > 0)
        {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(muted(), tr("Also keeping sliders on: %s"), others.c_str());
            ImGui::PopTextWrapPos();
            tooltip(tr("These characters keep their own sliders while you edit this one.\n"
                    "Pick one above to change it, or release it in the Status tab (Edited characters)."));
        }
    }

    bool PanelView::is_edited(const std::string& key) const
    {
        return std::any_of(m_edited.begin(), m_edited.end(), [&](const ActorSummary& a) { return a.key == key; });
    }

    // Status tab: every character with sliders, with Select / Release.
    void PanelView::draw_edited_characters()
    {
        Skin& sk = skin();
        ImGui::SeparatorText(tr("Edited characters"));
        if (!m_remembered.empty())
        {
            std::string list;
            for (const std::string& id : m_remembered)
            {
                list += (list.empty() ? "" : ", ") + id;
            }
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(muted(), tr("Remembered NPCs (%zu): %s"), m_remembered.size(), list.c_str());
            ImGui::PopTextWrapPos();
            tooltip(tr("Their sliders come back automatically when they are loaded within CharacterRange of the player.\n"
                    "Files: _characters folder inside the presets folder."));
        }
        if (m_edited.empty())
        {
            ImGui::TextColored(muted(), tr("None yet. A character gets its own sliders once you pick it and move one."));
            return;
        }
        std::string release_key;
        if (ImGui::BeginTable("##edited", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn(tr("Character"), ImGuiTableColumnFlags_WidthStretch, 3.0f);
            ImGui::TableSetupColumn(tr("Bones"), ImGuiTableColumnFlags_WidthStretch, 0.8f);
            ImGui::TableSetupColumn(tr("Morphs"), ImGuiTableColumnFlags_WidthStretch, 0.8f);
            ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed,
                                    sk.button_width(tr("Select")) + sk.button_width(tr("Release"), "reset") + ImGui::GetStyle().ItemSpacing.x);
            ImGui::TableHeadersRow();
            for (const ActorSummary& a : m_edited)
            {
                ImGui::PushID(a.key.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                if (a.active)
                {
                    ImGui::TextColored(accent(), tr("%s (selected)"), a.label.c_str());
                }
                else
                {
                    ImGui::TextUnformatted(a.label.c_str());
                }
                if (a.key == kPlayerActor)
                {
                    tooltip(tr("The player's sliders are saved with the session (_last_session)."));
                }
                else if (!a.identity.empty())
                {
                    ImGui::SameLine();
                    ImGui::TextColored(muted(), tr("- remembered"));
                    const std::string tip = std::string(tr("Remembered as \"")) + a.identity + tr("\": these sliders come back by themselves whenever this character\n"
                                            "is loaded near the player (after a reload, in a new area...). Release forgets it.");
                    tooltip(tip.c_str());
                }
                else
                {
                    ImGui::SameLine();
                    ImGui::TextColored(warn(), tr("- this visit only"));
                    tooltip(tr("This character has no stable name to recognise it by after a reload, so its sliders last until it despawns.\n"
                            "Save a preset to reuse them."));
                }
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::Text(tr("%zu"), a.bones);
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::Text(tr("%zu"), a.morphs);
                ImGui::TableNextColumn();
                ImGui::BeginDisabled(a.active || !m_targets);
                if (sk.button(tr("Select")))
                {
                    m_targets->pick_target(a.pick_id);
                    set_message(tr("Switching to " )+ a.label);
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (sk.button(tr("Release"), "reset"))
                {
                    release_key = a.key;
                }
                tooltip(tr("Drop this character's sliders. Its body goes back to normal, and it is left alone from now on\n"
                        "(unless you pick it and edit it again)."));
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (!release_key.empty())
        {
            Registry::instance().forget_actor(release_key);
            m_edited = Registry::instance().edited_actors();
            set_message(tr("Sliders released"));
        }
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(muted(), tr("The player's sliders are saved with the session. NPCs are remembered by name and get their sliders "
                                    "back whenever they are loaded near the player; Release or Reset all forgets them."));
        ImGui::PopTextWrapPos();
    }

    void PanelView::draw_simplified()
    {
        Skin& sk = skin();
        sk.begin_child("##simplified", ImVec2(0, 0), ImGuiChildFlags_None);
        if (m_skeleton.names.empty())
        {
            ImGui::TextWrapped(tr("The sliders appear once a character's skeletal mesh has been found. Load a save or enter a level, or pick a character above."));
            sk.end_child();
            return;
        }

        if (m_body.groups.empty())
        {
            ImGui::TextWrapped(tr("This skeleton uses bone names the Simplified Panel does not recognise. Every bone can still be edited in the Detailed Panel."));
        }
        const float icon_w = ImGui::GetFontSize();
        const float icon_room = icon_w + ImGui::GetStyle().ItemSpacing.x;
        float label_w = ImGui::CalcTextSize(tr("Upper arms")).x;
        for (const GroupBones& gb : m_body.groups)
        {
            label_w = std::max(label_w, ImGui::CalcTextSize(gb.group.title.c_str()).x + (sk.has_icon("group." + gb.group.title) ? icon_room : 0.0f));
        }
        for (const MorphGroupMorphs& mg : m_morph_groups)
        {
            label_w = std::max(label_w, ImGui::CalcTextSize(mg.group.title.c_str()).x + (sk.has_icon("group." + mg.group.title) ? icon_room : 0.0f));
        }
        label_w = std::min(label_w, ImGui::GetContentRegionAvail().x * 0.35f) + ImGui::GetStyle().ItemSpacing.x * 2;
        const float buttons = sk.button_width(tr("XYZ"), "xyz") + sk.button_width(tr("Reset"), "reset") + ImGui::GetStyle().ItemSpacing.x * 2;
        std::string section;
        bool first_section = true;
        for (const GroupBones& gb : m_body.groups)
        {
            const BodyGroup& g = gb.group;
            const std::string& first = gb.bones.front();
            if (first_section || section != g.section)
            {
                first_section = false;
                section = g.section;
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, muted());
                ImGui::SeparatorText(section.c_str());
                ImGui::PopStyleColor();
            }

            ImGui::PushID(g.title.c_str());
            BoneEdit s = scale_of(first, g.spread);
            const bool edited = s.has_scale();

            sk.icon("group." + g.title, icon_w);
            ImGui::AlignTextToFramePadding();
            if (edited)
            {
                ImGui::TextColored(accent(), tr("%s"), g.title.c_str());
            }
            else
            {
                ImGui::TextUnformatted(g.title.c_str());
            }
            {
                std::string tip = g.hint;
                tip += tr("\nBones:");
                for (const std::string& b : gb.bones)
                {
                    tip += " " + b;
                }
                tooltip(tip.c_str());
            }

            bool& open = m_group_open[g.title];
            ImGui::SameLine(label_w);
            ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x - buttons));
            if (size_slider("##size", s, kScaleMin, kScaleMax))
            {
                write_group(gb, s.axis);
            }
            ImGui::SameLine();
            const bool was_open = open;
            if (was_open)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, accent_dim());
            }
            if (sk.button(tr("XYZ"), "xyz"))
            {
                open = !open;
            }
            if (was_open)
            {
                ImGui::PopStyleColor();
            }
            tooltip(tr("Edit length / width / depth separately"));
            ImGui::SameLine();
            ImGui::BeginDisabled(!edited);
            if (sk.button(tr("Reset"), "reset"))
            {
                const double one[3] = {1.0, 1.0, 1.0};
                write_group(gb, one);
            }
            ImGui::EndDisabled();

            if (open)
            {
                ImGui::Indent(label_w);
                if (axis_sliders(s))
                {
                    write_group(gb, s.axis);
                }
                ImGui::Unindent(label_w);
            }
            ImGui::PopID();
        }
        draw_simplified_morphs(label_w);
        ImGui::Spacing();
        ImGui::TextColored(muted(), tr("Ctrl+click a slider to type a value. Ctrl+wheel nudges it (add Shift for finer steps)."));
        sk.end_child();
    }

    // "MorphGroups" from the game profile: one slider per group, driving every morph in it.
    void PanelView::draw_simplified_morphs(float label_w)
    {
        Skin& sk = skin();
        if (m_morph_groups.empty())
        {
            if (!m_morph_names.empty())
            {
                ImGui::Spacing();
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(muted(), tr("%zu morph target%s: see Detailed Panel > Morphs. Add \"MorphGroups\" to the game profile to put sliders here."),
                                   m_morph_names.size(), m_morph_names.size() == 1 ? "" : "s");
                ImGui::PopTextWrapPos();
            }
            return;
        }
        const float icon_w = ImGui::GetFontSize();
        const float reset_w = sk.button_width(tr("Reset"), "reset") + ImGui::GetStyle().ItemSpacing.x;
        std::string section;
        bool first = true;
        for (const MorphGroupMorphs& mg : m_morph_groups)
        {
            const std::string sec = mg.group.section.empty() ? std::string(tr("Morphs")) : mg.group.section;
            if (first || sec != section)
            {
                first = false;
                section = sec;
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, muted());
                ImGui::SeparatorText(section.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::PushID(("morphgroup:" + mg.group.title).c_str());
            std::optional<double> current;
            for (const std::string& m : mg.morphs)
            {
                if (const auto w = morph_of(m))
                {
                    current = w;
                    break;
                }
            }
            const bool edited = current.has_value();
            sk.icon("group." + mg.group.title, icon_w);
            ImGui::AlignTextToFramePadding();
            if (edited)
            {
                ImGui::TextColored(accent(), tr("%s"), mg.group.title.c_str());
            }
            else
            {
                ImGui::TextUnformatted(mg.group.title.c_str());
            }
            {
                std::string tip = mg.group.hint;
                tip += tip.empty() ? tr("Morphs:") : tr("\nMorphs:");
                for (const std::string& m : mg.morphs)
                {
                    tip += " " + m;
                }
                tooltip(tip.c_str());
            }
            ImGui::SameLine(label_w);
            ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x - reset_w));
            float v = static_cast<float>(current.value_or(0.0));
            if (morph_slider("##w", v))
            {
                for (const std::string& m : mg.morphs)
                {
                    write_morph(m, v);
                }
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!edited);
            if (sk.button(tr("Reset"), "reset"))
            {
                for (const std::string& m : mg.morphs)
                {
                    write_morph(m, std::nullopt);
                }
            }
            ImGui::EndDisabled();
            tooltip(tr("Give these morphs back to the game (its own value comes back)."));
            ImGui::PopID();
        }
    }

    void PanelView::draw_detailed()
    {
        if (m_morph_names.empty())
        {
            draw_detailed_bones();
            return;
        }
        Skin& sk = skin();
        if (ImGui::BeginTabBar("##detail"))
        {
            auto detail_flags = [this](Detail d) {
                return m_focus_detail == static_cast<int>(d) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            };
            char label[64];
            std::snprintf(label, sizeof(label), tr("Bones (%zu)###bones"), m_skeleton.names.size());
            if (sk.begin_tab_item(label, "bones", detail_flags(Detail::Bones)))
            {
                draw_detailed_bones();
                ImGui::EndTabItem();
            }
            std::snprintf(label, sizeof(label), tr("Morphs (%zu)###morphs"), m_morph_names.size());
            if (sk.begin_tab_item(label, "morphs", detail_flags(Detail::Morphs)))
            {
                draw_detailed_morphs();
                ImGui::EndTabItem();
            }
            m_focus_detail = -1;
            ImGui::EndTabBar();
        }
    }

    void PanelView::draw_detailed_bones()
    {
        Skin& sk = skin();
        if (m_skeleton.names.empty())
        {
            ImGui::TextWrapped(tr("No bones yet - no character mesh has been found."));
            return;
        }
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(tr("Edited only")).x - ImGui::GetFrameHeight() -
                                ImGui::GetStyle().ItemSpacing.x * 2);
        ImGui::InputTextWithHint("##find", tr("Search bones..."), m_filter, sizeof(m_filter));
        ImGui::SameLine();
        sk.checkbox(tr("Edited only"), &m_only_edited);

        const std::string needle = fold_case(m_filter);
        const float list_h = std::max(140.0f, ImGui::GetContentRegionAvail().y * 0.42f);
        sk.begin_child("##bonelist", ImVec2(0, list_h), ImGuiChildFlags_Borders);
        const float indent = ImGui::GetFontSize() * 0.75f;
        const EditBook& book = active_book();
        for (size_t i = 0; i < m_skeleton.names.size(); ++i)
        {
            const std::string& name = m_skeleton.names[i];
            const auto it = book.find(fold_case(name));
            const bool edited = it != book.end() && !it->second.edit.is_neutral();
            if (m_only_edited && !edited)
            {
                continue;
            }
            if (!needle.empty() && fold_case(name).find(needle) == std::string::npos)
            {
                continue;
            }
            if (needle.empty() && !m_only_edited)
            {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent * static_cast<float>(m_skeleton.depth[i]));
            }
            char label[256];
            const auto grp = m_bone_group.find(fold_case(name));
            const std::string part = grp != m_bone_group.end() ? "   [" + grp->second + "]" : std::string();
            if (edited)
            {
                const BoneEdit& e = it->second.edit;
                std::string tags;
                if (e.has_scale())
                {
                    char buf[48];
                    std::snprintf(buf, sizeof(buf), "  %.2f/%.2f/%.2f", e.axis[0], e.axis[1], e.axis[2]);
                    tags += buf;
                }
                if (e.has_turn())
                {
                    tags += tr("  rot");
                }
                if (e.has_shift())
                {
                    tags += tr("  move");
                }
                std::snprintf(label, sizeof(label), "%s%s %s##%zu", name.c_str(), part.c_str(), tags.c_str(), i);
                ImGui::PushStyleColor(ImGuiCol_Text, accent());
            }
            else
            {
                std::snprintf(label, sizeof(label), "%s%s##%zu", name.c_str(), part.c_str(), i);
            }
            if (ImGui::Selectable(label, fold_case(m_selected) == fold_case(name)))
            {
                m_selected = name;
            }
            if (edited)
            {
                ImGui::PopStyleColor();
            }
        }
        sk.end_child();

        ImGui::Spacing();
        if (m_selected.empty() || !has_bone(m_selected))
        {
            ImGui::TextColored(muted(), tr("Pick a bone above to edit it."));
            return;
        }

        const std::string mirror = mirror_bone_name(m_selected);
        const bool can_mirror = !mirror.empty() && has_bone(mirror);
        ImGui::TextColored(accent(), tr("%s"), m_selected.c_str());
        if (can_mirror && m_mirror)
        {
            ImGui::SameLine();
            ImGui::TextColored(muted(), tr("+ %s"), mirror.c_str());
        }

        BoneEdit s = scale_of(m_selected, Spread::Chain);
        bool changed = false;
        sk.begin_child("##editor", ImVec2(0, 0), ImGuiChildFlags_None);
        if (ImGui::CollapsingHeader(s.has_scale() ? tr("Scale  *###scale") : tr("Scale###scale"), ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::SetNextItemWidth(-ImGui::CalcTextSize(tr("Length (X)")).x - ImGui::GetStyle().ItemInnerSpacing.x * 2);
            changed |= size_slider("Size", s, kScaleMin, kScaleMax);
            changed |= axis_sliders(s);
            if (s.has_scale() && sk.small_button(tr("Reset scale")))
            {
                s.axis[0] = s.axis[1] = s.axis[2] = 1.0;
                changed = true;
            }
        }
        if (ImGui::CollapsingHeader(s.has_turn() ? tr("Rotate  *###rotate") : tr("Rotate###rotate"), s.has_turn() ? ImGuiTreeNodeFlags_DefaultOpen : 0))
        {
            changed |= turn_sliders(s);
            if (s.has_turn() && sk.small_button(tr("Reset rotation")))
            {
                s.turn[0] = s.turn[1] = s.turn[2] = 0.0;
                changed = true;
            }
        }
        if (ImGui::CollapsingHeader(s.has_shift() ? tr("Move  *###move") : tr("Move###move"), s.has_shift() ? ImGuiTreeNodeFlags_DefaultOpen : 0))
        {
            changed |= shift_sliders(s);
            if (s.has_shift() && sk.small_button(tr("Reset move")))
            {
                s.shift[0] = s.shift[1] = s.shift[2] = 0.0;
                changed = true;
            }
        }
        ImGui::Spacing();

        int spread = static_cast<int>(s.spread);
        const char* spreads[3] = {tr(spread_label(Spread::Chain)), tr(spread_label(Spread::Keep)), tr(spread_label(Spread::Solo))};
        ImGui::SetNextItemWidth(-ImGui::CalcTextSize(tr("Length (X)")).x - ImGui::GetStyle().ItemInnerSpacing.x * 2);
        if (ImGui::Combo(tr("Children"), &spread, spreads, 3))
        {
            s.spread = static_cast<Spread>(spread);
            changed = true; // remembered as a hint even while the bone is still at 1.0x
        }
        tooltip(tr("Scale children too: children follow the bone's scale, rotation and move.\n"
                "Keep children size: children follow rotation/move and the new shape but keep their own size.\n"
                "This bone only: nothing below it changes (rotation/move included)."));

        if (can_mirror)
        {
            sk.checkbox(tr("Mirror to other side"), &m_mirror);
            ImGui::SameLine();
        }
        if (sk.button(tr("Reset bone"), "reset"))
        {
            BoneEdit neutral;
            neutral.spread = s.spread;
            s = neutral;
            changed = true;
        }

        ImGui::TextColored(muted(), tr("Ctrl+click to type a value. Ctrl+wheel nudges (Shift: finer)."));
        sk.end_child();

        if (changed)
        {
            write(m_selected, s);
            if (can_mirror && m_mirror)
            {
                write(mirror, Registry::instance().mirror_edit(m_selected, s)); // mirror-image rotation/move, measured from the skeleton
            }
        }
    }

    void PanelView::draw_detailed_morphs()
    {
        Skin& sk = skin();
        const float reset_w = sk.button_width(tr("Reset"), "reset");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(tr("Edited only")).x - ImGui::GetFrameHeight() -
                                ImGui::GetStyle().ItemSpacing.x * 2);
        ImGui::InputTextWithHint("##findmorph", tr("Search morphs..."), m_morph_filter, sizeof(m_morph_filter));
        ImGui::SameLine();
        sk.checkbox(tr("Edited only"), &m_morph_only_edited);

        const std::string needle = fold_case(m_morph_filter);
        std::vector<const std::string*> rows;
        rows.reserve(m_morph_names.size());
        for (const std::string& m : m_morph_names)
        {
            if (m_morph_only_edited && !morph_of(m))
            {
                continue;
            }
            if (!contains_folded(m, needle))
            {
                continue;
            }
            rows.push_back(&m);
        }

        const float footer = ImGui::GetFrameHeightWithSpacing() * 2.2f;
        sk.begin_child("##morphlist", ImVec2(0, std::max(140.0f, ImGui::GetContentRegionAvail().y - footer)), ImGuiChildFlags_Borders);
        float name_w = ImGui::CalcTextSize(tr("MMMMMMMMMMMM")).x;
        for (const std::string* m : rows)
        {
            name_w = std::max(name_w, ImGui::CalcTextSize(m->c_str()).x);
        }
        const float warn_w = ImGui::CalcTextSize(tr(" !")).x;
        name_w = std::min(name_w + warn_w, ImGui::GetContentRegionAvail().x * 0.45f) + ImGui::GetStyle().ItemSpacing.x;

        ImGuiListClipper clipper; // faces can have hundreds of morphs
        clipper.Begin(static_cast<int>(rows.size()), ImGui::GetFrameHeightWithSpacing());
        while (clipper.Step())
        {
            for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r)
            {
                const std::string& name = *rows[static_cast<size_t>(r)];
                ImGui::PushID(name.c_str());
                const std::optional<double> w = morph_of(name);
                const bool animated = m_animated.count(fold_case(name)) > 0;
                ImGui::AlignTextToFramePadding();
                if (w)
                {
                    ImGui::TextColored(accent(), tr("%s"), name.c_str());
                }
                else
                {
                    ImGui::TextUnformatted(name.c_str());
                }
                if (animated)
                {
                    ImGui::SameLine(0.0f, 0.0f);
                    ImGui::TextColored(warn(), tr(" !"));
                    tooltip(tr("The game keeps setting this morph itself (animation or its own code),\nso the slider may not stick. UUEPBS re-applies it each time it notices."));
                }
                ImGui::SameLine(name_w);
                ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x - reset_w - ImGui::GetStyle().ItemSpacing.x));
                float v = static_cast<float>(w.value_or(0.0));
                if (morph_slider("##w", v))
                {
                    write_morph(name, v);
                    const std::string other = mirror_bone_name(name);
                    if (m_mirror && !other.empty() && has_morph(other))
                    {
                        write_morph(other, v);
                    }
                }
                ImGui::SameLine();
                ImGui::BeginDisabled(!w);
                if (sk.button(tr("Reset"), "reset"))
                {
                    write_morph(name, std::nullopt);
                    const std::string other = mirror_bone_name(name);
                    if (m_mirror && !other.empty() && has_morph(other))
                    {
                        write_morph(other, std::nullopt);
                    }
                }
                ImGui::EndDisabled();
                ImGui::PopID();
            }
        }
        if (rows.empty())
        {
            ImGui::TextColored(muted(), tr(m_morph_only_edited ? "No morph is set yet." : "No morph matches the search."));
        }
        sk.end_child();

        sk.checkbox(tr("Mirror left/right morphs"), &m_mirror);
        tooltip(tr("Moving a _L morph also moves its _R twin (and the other way round)."));
        ImGui::SameLine();
        ImGui::BeginDisabled(m_morphs.empty());
        if (sk.button(tr("Reset all morphs"), "reset"))
        {
            Registry::instance().clear_morphs();
            m_morphs.clear();
            m_morph_revision = Registry::instance().morph_revision();
            set_message(tr("All morphs given back to the game"));
        }
        ImGui::EndDisabled();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(muted(), tr("A set morph replaces the game's value (even at 0); Reset gives it back. Ctrl+click to type values outside 0..1."));
        ImGui::PopTextWrapPos();
    }

    void PanelView::refresh_presets()
    {
        if (m_shelf)
        {
            m_presets = m_shelf->list();
        }
        m_presets_time = Clock::now();
    }

    void PanelView::load_preset(const std::string& name)
    {
        EditBook book;
        MorphBook morphs;
        LocoBookMap loco;
        std::string message;
        if (m_shelf && m_shelf->load(name, book, &loco, message, &morphs))
        {
            Registry::instance().replace_edits(std::move(book));
            Registry::instance().replace_morphs(std::move(morphs));
            Registry::instance().replace_loco_all(std::move(loco));
            std::snprintf(m_preset_name, sizeof(m_preset_name), "%s", name.c_str());
        }
        set_message(message);
    }

    void PanelView::save_preset(const std::string& name)
    {
        std::string message;
        if (m_shelf)
        {
            const MorphBook morphs = Registry::instance().morphs();
            m_shelf->save(name, Registry::instance().edits(), Registry::instance().loco_all(), message, &morphs);
        }
        set_message(message);
        refresh_presets();
    }

    // "BP_Weapon_Hun_PoJun" -> "PoJun" (context keys stay untouched; this is only for display).
    static std::string loco_context_label(const std::string& key)
    {
        if (key == "unarmed")
        {
            return tr("Unarmed (sheathed)");
        }
        if (key == "cutscene")
        {
            // 过场（剧情演出）专用档：状态固定待机，和武器档完全分开。
            return tr("Cutscene (story scene)");
        }
        if (key.rfind("weapon_", 0) == 0)
        {
            // EWeaponType in the game binary: None=0, Blade=1, DoubleBlade=2, GreatBlade=3.
            struct TypeName
            {
                int value;
                const char* english;
                const char* chinese;
            };
            static const TypeName names[] = {
                {0, "None", "无"},
                {1, "Blade", "刀"},
                {2, "DoubleBlade", "双刀"},
                {3, "GreatBlade", "巨刃"},
            };
            const int value = std::atoi(key.c_str() + 7);
            const bool zh = language() == Language::Chinese;
            for (const TypeName& entry : names)
            {
                if (entry.value == value)
                {
                    return zh ? entry.chinese : entry.english;
                }
            }
            return std::string(zh ? "武器类型 " : "Weapon type ") + key.substr(7);
        }
        std::string label = key;
        static const char* const prefixes[] = {"BP_ShaqiWeapon_", "BP_Weapon_", "BP_"};
        for (const char* prefix : prefixes)
        {
            const size_t n = std::char_traits<char>::length(prefix);
            if (label.rfind(prefix, 0) == 0)
            {
                label = label.substr(n);
                break;
            }
        }
        for (char& c : label)
        {
            if (c == '_')
            {
                c = ' ';
            }
        }
        return label;
    }

    void PanelView::draw_locomotion()
    {
        Skin& sk = skin();
        m_loco_seen = true;
        const char* const names[uuepbs::kLocoStateCount] = {tr("Idle"), tr("Walk"), tr("Run"), tr("Sprint")};
        ImGui::TextWrapped(
            tr("Bone offsets per locomotion state. They are added on top of the Detailed Panel edits and apply only while "
            "the layer is on: weapon on the back (unarmed) or drawn (one context per weapon type), and no action playing.\n"
            "Story sequences use their own Cutscene entry (state pinned to Idle)."));
        ImGui::Spacing();

        const int live = (m_loco_live >= 0 && m_loco_live < uuepbs::kLocoStateCount) ? m_loco_live : -1;
        ImGui::TextColored(accent(), tr("Now: %s"), live >= 0 ? names[live] : tr("unknown"));
        ImGui::SameLine();
        ImGui::TextColored(muted(), tr("· %s · layer %s"), loco_context_label(m_loco_live_context).c_str(), m_loco_layer_on ? tr("on") : tr("off"));
        if (Registry::instance().cinematic())
        {
            ImGui::SameLine();
            ImGui::TextColored(accent(), tr("· cutscene, layer kept on"));
        }
        ImGui::Spacing();

        ImGui::TextUnformatted(tr("Weapon context:"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(260.0f);
        const std::string current_label = loco_context_label(m_loco_context);
        if (ImGui::BeginCombo("##lococontext", current_label.c_str()))
        {
            if (ImGui::Selectable(tr("Unarmed (sheathed)"), m_loco_context == "unarmed"))
            {
                m_loco_context = "unarmed";
            }
            // 过场档固定排在第二位：不用等它真的激活过就能直接调。
            const std::string cutscene_label = tr("Cutscene (story scene)");
            if (ImGui::Selectable((cutscene_label + "###ctx_cutscene").c_str(), m_loco_context == "cutscene"))
            {
                m_loco_context = "cutscene";
            }
            for (const std::string& key : m_loco_known)
            {
                if (key == "unarmed" || key == "cutscene")
                {
                    continue;
                }
                const std::string label = loco_context_label(key);
                if (ImGui::Selectable((label + "###ctx_" + key).c_str(), m_loco_context == key))
                {
                    m_loco_context = key;
                }
            }
            ImGui::EndCombo();
        }
        tooltip(tr("Unarmed = weapon on the back. The other entries are weapon types you have drawn once;\n"
                "each one keeps its own Idle/Walk/Run/Sprint offsets.\n"
                "Cutscene = used while a story sequence plays; its state is pinned to Idle."));

        ImGui::Spacing();
        ImGui::TextUnformatted(tr("Editing:"));
        for (int i = 0; i < uuepbs::kLocoStateCount; ++i)
        {
            if (i > 0)
            {
                ImGui::SameLine();
            }
            if (ImGui::RadioButton(names[i], m_loco_state == i))
            {
                m_loco_state = i;
            }
        }
        ImGui::SameLine();
        if (sk.small_button(tr("Clear this state")))
        {
            Registry::instance().replace_loco_book(m_loco_context, m_loco_state, EditBook{});
            m_loco_ctx[m_loco_context][static_cast<size_t>(m_loco_state)].clear();
            m_loco_revision = Registry::instance().loco_revision();
        }
        tooltip(tr("Removes every offset of the selected context + state. Everything else keeps its values."));

        sk.checkbox(tr("Preview this context/state"), &m_loco_preview);
        tooltip(tr("Applies the selected context/state while you stand still, so the sliders can be tuned\n"
                "without running or switching weapons. Only active while this tab is open."));
        ImGui::SameLine();
        ImGui::TextColored(muted(), tr(m_loco_preview ? "preview forcing this context on" : "preview off"));
        Registry::instance().set_loco_preview(m_loco_preview ? m_loco_context : std::string(), m_loco_preview ? m_loco_state : -1);

        int blend = Registry::instance().loco_blend_ms();
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::SliderInt(tr("Blend (ms)"), &blend, 0, 1000))
        {
            Registry::instance().set_loco_blend_ms(blend);
        }
        tooltip(tr("Cross-fade time when the context (unarmed / weapon), the state or the on/off flag changes.\n"
                "0 = snap instantly. The default comes from LocomotionBlendMs in config.lua."));
        ImGui::SameLine();
        ImGui::TextColored(muted(), tr("applies to context/state changes"));
        ImGui::Separator();

        // The bone editor below is the same one the Detailed Panel uses; it just edits the
        // selected context + state's book instead of the normal sliders.
        m_loco_edit = true;
        draw_detailed_bones();
        m_loco_edit = false;
    }

    // ---- 时间轴动画（眨眼这类小动作）-----------------------------------------
    void PanelView::load_animations()
    {
        if (!m_shelf || !m_shelf->ready())
        {
            return;
        }
        std::vector<std::string> errors;
        m_anims = anim_load_all(m_shelf->folder(), &errors);
        if (!errors.empty())
        {
            m_anim_message = errors.front();
        }
        if (m_anim_selected.empty() && !m_anims.empty())
        {
            m_anim_selected = m_anims.front().name;
        }
        Registry::instance().set_animations(m_anims);
    }

    Animation* PanelView::selected_animation()
    {
        for (Animation& animation : m_anims)
        {
            if (animation.name == m_anim_selected)
            {
                return &animation;
            }
        }
        return nullptr;
    }

    void PanelView::anim_changed(const std::string& animation_name, bool structural)
    {
        if (!animation_name.empty())
        {
            m_anim_dirty.insert(animation_name);
        }
        // 只推给 Registry（游戏里立刻能预览），不再自动写盘；保存由「保存」按钮手动触发。
        Registry::instance().set_animations(m_anims);
        if (structural)
        {
            Registry::instance().restart_animations();
        }
    }

    void PanelView::load_animation_from_disk(Animation& animation)
    {
        if (!m_shelf || !m_shelf->ready() || animation.name.empty())
        {
            return;
        }
        Animation loaded;
        std::string message;
        if (!anim_load(m_shelf->folder(), animation.name, loaded, message))
        {
            m_anim_message = message;
            return;
        }
        loaded.name = animation.name;
        animation = loaded;
        m_anim_dirty.erase(animation.name);
        Registry::instance().set_animations(m_anims);
        Registry::instance().restart_animations();
        m_anim_message = message;
    }

    // 一条轨道：骨骼、循环、子级联动、节点列表、时间轴预览。
    void PanelView::anim_draw_track(Animation& animation, size_t track_index)
    {
        Skin& sk = skin();
        AnimTrack& track = animation.tracks[track_index];
        const size_t anim_index = static_cast<size_t>(&animation - m_anims.data());

        auto node_seconds = [](const AnimNode& node) {
            if (node.kind == AnimNodeKind::Move)
            {
                return std::max(0.001, node.duration);
            }
            if (node.random)
            {
                const double lo = std::min(node.wait_min, node.wait_max);
                const double hi = std::max(node.wait_min, node.wait_max);
                return std::max(0.0, (lo + hi) * 0.5);
            }
            return std::max(0.0, node.wait);
        };

        ImGui::PushID(static_cast<int>(track_index));
        ImGui::BeginChild(("##animtrack" + std::to_string(track_index)).c_str(), ImVec2(0.0f, 0.0f),
                          ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);

        ImGui::Text(tr("Track %d"), static_cast<int>(track_index) + 1);
        ImGui::SameLine();
        bool enabled = track.enabled;
        if (sk.checkbox(tr("Enabled"), &enabled))
        {
            track.enabled = enabled;
            anim_changed(animation.name, false);
        }
        ImGui::SameLine();
        const std::string bones_label = track.bones.empty()
                                            ? std::string(tr("(no bones)"))
                                            : std::to_string(track.bones.size()) + " " + tr("bone(s)");
        if (sk.small_button(bones_label.c_str()))
        {
            ImGui::OpenPopup("bonepick");
        }
        if (ImGui::BeginPopup("bonepick"))
        {
            ImGui::SetNextItemWidth(280.0f);
            ImGui::InputTextWithHint("##bonefilter", tr("Search bones..."), m_anim_bone_filter, sizeof(m_anim_bone_filter));
            const std::string needle = fold_case(m_anim_bone_filter);
            ImGui::BeginChild("##bonelist", ImVec2(300.0f, 300.0f));
            int shown = 0;
            for (const std::string& bone : m_skeleton.names)
            {
                if (!contains_folded(bone, needle))
                {
                    continue;
                }
                if (++shown > 400)
                {
                    break;
                }
                bool selected = false;
                for (const std::string& current : track.bones)
                {
                    if (fold_case(current) == fold_case(bone))
                    {
                        selected = true;
                        break;
                    }
                }
                if (ImGui::Selectable(bone.c_str(), selected))
                {
                    if (selected)
                    {
                        for (auto it = track.bones.begin(); it != track.bones.end(); ++it)
                        {
                            if (fold_case(*it) == fold_case(bone))
                            {
                                track.bones.erase(it);
                                break;
                            }
                        }
                    }
                    else
                    {
                        track.bones.push_back(bone);
                    }
                    anim_changed(animation.name, true);
                }
            }
            ImGui::EndChild();
            if (sk.small_button(tr("Clear bones")))
            {
                track.bones.clear();
                anim_changed(animation.name, true);
            }
            ImGui::SameLine();
            ImGui::TextColored(muted(), tr("Select any number of bones."));
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        bool loop = track.loop;
        if (sk.checkbox(tr("Loop"), &loop))
        {
            track.loop = loop;
            anim_changed(animation.name, false);
        }
        ImGui::SameLine(0.0f, 14.0f);
        const char* const follows[3] = {tr(spread_label(Spread::Chain)), tr(spread_label(Spread::Keep)), tr(spread_label(Spread::Solo))};
        int spread = static_cast<int>(track.spread);
        ImGui::SetNextItemWidth(170.0f);
        if (ImGui::Combo("##follow", &spread, follows, 3))
        {
            track.spread = static_cast<Spread>(spread);
            anim_changed(animation.name, false);
        }
        ImGui::SameLine();
        if (sk.small_button(tr("Remove track")))
        {
            animation.tracks.erase(animation.tracks.begin() + static_cast<std::ptrdiff_t>(track_index));
            anim_changed(animation.name, true);
            ImGui::EndChild();
            ImGui::PopID();
            return;
        }

        // 时间轴预览：每段宽度 = 节点时长，播放头来自 Registry 的实时进度。
        {
            const double cycle = anim_track_cycle(track);
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const float width = std::max(120.0f, ImGui::GetContentRegionAvail().x);
            const float height = 10.0f;
            ImGui::Dummy(ImVec2(width, height));
            ImDrawList* draw = ImGui::GetWindowDrawList();
            auto color32 = [](const ImVec4& color, float alpha) {
                ImVec4 copy = color;
                copy.w *= alpha;
                return ImGui::GetColorU32(copy);
            };
            draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), color32(muted(), 0.18f), 3.0f);
            if (cycle > 0.0001)
            {
                double at = 0.0;
                for (const AnimNode& node : track.nodes)
                {
                    const double seconds = node_seconds(node);
                    const float x0 = origin.x + static_cast<float>(at / cycle) * width;
                    const float x1 = origin.x + static_cast<float>((at + seconds) / cycle) * width;
                    const ImU32 color = node.kind == AnimNodeKind::Move ? color32(accent(), 0.55f) : color32(muted(), 0.40f);
                    draw->AddRectFilled(ImVec2(x0, origin.y), ImVec2(std::max(x1, x0 + 1.0f), origin.y + height), color, 3.0f);
                    at += seconds;
                }
                // 播放头：当前节点之前的总时长 + 节点内进度。
                if (anim_index < m_anim_live.size() && track_index < m_anim_live[anim_index].size())
                {
                    const Registry::AnimLiveTrack& live = m_anim_live[anim_index][track_index];
                    if (live.node >= 0 && live.node < static_cast<int>(track.nodes.size()))
                    {
                        double elapsed = 0.0;
                        for (int i = 0; i < live.node; ++i)
                        {
                            elapsed += node_seconds(track.nodes[static_cast<size_t>(i)]);
                        }
                        elapsed += live.progress * node_seconds(track.nodes[static_cast<size_t>(live.node)]);
                        const float x = origin.x + static_cast<float>(elapsed / cycle) * width;
                        draw->AddRectFilled(ImVec2(x - 1.0f, origin.y - 2.0f), ImVec2(x + 1.5f, origin.y + height + 2.0f),
                                            color32(good(), 0.95f), 1.0f);
                    }
                }
            }
        }
        ImGui::Spacing();

        if (track.bones.empty())
        {
            ImGui::TextColored(warn(), tr("Pick the bones this track drives."));
        }

        // 节点列表
        // 三行数值（旋转/位移/缩放）共用一个 lambda，所以每一行要 PushID(label)，
        // 否则三行里的 X/Y/Z 控件会拿到同一个 ImGui ID（悬浮时会弹红色 ID 冲突报错）。
        auto vec3 = [&](const char* label, double values[3], float speed, float lo, float hi, const char* format) {
            ImGui::PushID(label);
            ImGui::TextUnformatted(label);
            bool changed = false;
            for (int i = 0; i < 3; ++i)
            {
                ImGui::SameLine();
                ImGui::PushID(i);
                ImGui::SetNextItemWidth(96.0f);
                float value = static_cast<float>(values[i]);
                if (ImGui::DragFloat("##axis", &value, speed, lo, hi, format))
                {
                    values[i] = value;
                    changed = true;
                }
                ImGui::PopID();
            }
            ImGui::PopID();
            return changed;
        };

        for (size_t n = 0; n < track.nodes.size(); ++n)
        {
            AnimNode& node = track.nodes[n];
            bool restructured = false;
            ImGui::PushID(static_cast<int>(n));
            ImGui::Separator();

            const char* const kinds[2] = {tr("Move"), tr("Delay")};
            int kind = node.kind == AnimNodeKind::Move ? 0 : 1;
            ImGui::SetNextItemWidth(90.0f);
            if (ImGui::Combo("##kind", &kind, kinds, 2))
            {
                node.kind = kind == 0 ? AnimNodeKind::Move : AnimNodeKind::Delay;
                anim_changed(animation.name, true);
                restructured = true;
            }

            if (!restructured && node.kind == AnimNodeKind::Move)
            {
                ImGui::SameLine();
                ImGui::TextUnformatted(tr("Duration"));
                ImGui::SameLine();
                ImGui::SetNextItemWidth(96.0f);
                float duration = static_cast<float>(node.duration);
                if (ImGui::DragFloat("##duration", &duration, 0.005f, 0.01f, 30.0f, tr("%.2f s")))
                {
                    node.duration = duration;
                    anim_changed(animation.name, false);
                }
                ImGui::SameLine();
                const char* const eases[uuepbs::kEaseCount] = {tr("Linear"), tr("Smooth"), tr("Ease in"), tr("Ease out"),
                                                               tr("Ease in-out")};
                int ease = static_cast<int>(node.ease);
                ImGui::SetNextItemWidth(120.0f);
                if (ImGui::Combo("##ease", &ease, eases, uuepbs::kEaseCount))
                {
                    node.ease = static_cast<Ease>(ease);
                    anim_changed(animation.name, false);
                }
                if (vec3(tr("Rotation"), node.target.turn, 0.1f, -180.0f, 180.0f, "%.1f\xC2\xB0"))
                {
                    node.target.clamp();
                    anim_changed(animation.name, false);
                }
                if (vec3(tr("Translation"), node.target.shift, 0.01f, -20.0f, 20.0f, tr("%.2f cm")))
                {
                    node.target.clamp();
                    anim_changed(animation.name, false);
                }
                if (vec3(tr("Scale"), node.target.axis, 0.005f, 0.1f, 5.0f, "%.3f"))
                {
                    node.target.clamp();
                    anim_changed(animation.name, false);
                }
            }
            else if (!restructured)
            {
                ImGui::SameLine();
                const char* const modes[2] = {tr("Fixed"), tr("Random")};
                int mode = node.random ? 1 : 0;
                ImGui::SetNextItemWidth(96.0f);
                if (ImGui::Combo("##mode", &mode, modes, 2))
                {
                    node.random = mode == 1;
                    anim_changed(animation.name, true);
                    restructured = true;
                }
                if (!restructured)
                {
                    ImGui::SameLine();
                    if (!node.random)
                    {
                        ImGui::TextUnformatted(tr("Wait"));
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(96.0f);
                        float wait = static_cast<float>(node.wait);
                        if (ImGui::DragFloat("##wait", &wait, 0.05f, 0.0f, 600.0f, tr("%.2f s")))
                        {
                            node.wait = wait;
                            anim_changed(animation.name, false);
                        }
                    }
                    else
                    {
                        ImGui::TextUnformatted(tr("Min"));
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(96.0f);
                        float lo = static_cast<float>(node.wait_min);
                        if (ImGui::DragFloat("##min", &lo, 0.05f, 0.0f, 600.0f, tr("%.2f s")))
                        {
                            node.wait_min = lo;
                            node.wait_max = std::max(node.wait_max, node.wait_min);
                            anim_changed(animation.name, false);
                        }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(tr("Max"));
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(96.0f);
                        float hi = static_cast<float>(node.wait_max);
                        if (ImGui::DragFloat("##max", &hi, 0.05f, 0.0f, 600.0f, tr("%.2f s")))
                        {
                            node.wait_max = std::max(static_cast<double>(hi), node.wait_min);
                            anim_changed(animation.name, false);
                        }
                    }
                }
            }

            // 上移 / 下移 / 删除（改了就跳出循环，下一帧重新画）
            if (!restructured)
            {
                ImGui::SameLine();
                ImGui::BeginDisabled(n == 0);
                if (sk.small_button(tr("Up")))
                {
                    std::swap(track.nodes[n], track.nodes[n - 1]);
                    anim_changed(animation.name, true);
                    restructured = true;
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled(n + 1 >= track.nodes.size());
                if (sk.small_button(tr("Down")))
                {
                    std::swap(track.nodes[n], track.nodes[n + 1]);
                    anim_changed(animation.name, true);
                    restructured = true;
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (sk.small_button(tr("Delete")))
                {
                    track.nodes.erase(track.nodes.begin() + static_cast<std::ptrdiff_t>(n));
                    anim_changed(animation.name, true);
                    restructured = true;
                }
            }
            ImGui::PopID();
            if (restructured)
            {
                break;
            }
        }

        ImGui::Separator();
        if (sk.small_button(tr("+ Move node")))
        {
            AnimNode node;
            node.kind = AnimNodeKind::Move;
            track.nodes.push_back(node);
            anim_changed(animation.name, true);
        }
        ImGui::SameLine();
        if (sk.small_button(tr("+ Delay node")))
        {
            AnimNode node;
            node.kind = AnimNodeKind::Delay;
            track.nodes.push_back(node);
            anim_changed(animation.name, true);
        }

        ImGui::EndChild();
        ImGui::PopID();
    }

    void PanelView::draw_animations()
    {
        Skin& sk = skin();
        if (!m_shelf || !m_shelf->ready())
        {
            ImGui::TextColored(warn(), tr("Preset folder is not set up yet."));
            return;
        }
        if (!m_anim_loaded)
        {
            m_anim_loaded = true;
            load_animations();
        }

        ImGui::TextWrapped(
            tr("Timeline animations: pick a group of bones and chain nodes.\n"
               "A Move node blends those bones to a target over a set time (linear / smooth / ease in / ease out / ease in-out).\n"
               "A Delay node waits a fixed time, or a random time in a range, before the next node.\n"
               "Tracks loop, so a blink is: random delay -> close -> open -> loop. Only the player character is animated."));
        ImGui::Spacing();

        // 动画列表 + 新建
        std::string combo_preview = m_anim_selected.empty() ? std::string(tr("(none)")) : m_anim_selected;
        if (!m_anim_selected.empty() && m_anim_dirty.count(m_anim_selected) != 0)
        {
            combo_preview += " *";
        }
        ImGui::SetNextItemWidth(240.0f);
        if (ImGui::BeginCombo("##animlist", combo_preview.c_str()))
        {
            for (const Animation& animation : m_anims)
            {
                if (ImGui::Selectable(animation.name.c_str(), animation.name == m_anim_selected))
                {
                    m_anim_selected = animation.name;
                    m_anim_bone_filter[0] = 0;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputTextWithHint("##newanim", tr("New animation name"), m_anim_new_name, sizeof(m_anim_new_name));
        ImGui::SameLine();
        if (sk.button(tr("New")))
        {
            const std::string clean = PresetShelf::clean_name(m_anim_new_name);
            if (!clean.empty())
            {
                bool exists = false;
                for (const Animation& animation : m_anims)
                {
                    if (animation.name == clean)
                    {
                        exists = true;
                        break;
                    }
                }
                if (!exists)
                {
                    Animation animation;
                    animation.name = clean;
                    AnimTrack track;
                    AnimNode wait;
                    wait.kind = AnimNodeKind::Delay;
                    wait.random = true;
                    wait.wait_min = 2.0;
                    wait.wait_max = 5.0;
                    AnimNode close;
                    close.kind = AnimNodeKind::Move;
                    close.duration = 0.07;
                    close.ease = Ease::Out;
                    close.target.turn[2] = -14.0;
                    AnimNode open;
                    open.kind = AnimNodeKind::Move;
                    open.duration = 0.14;
                    open.ease = Ease::In;
                    track.nodes = {wait, close, open};
                    animation.tracks.push_back(track);
                    m_anims.push_back(animation);
                    m_anim_selected = clean;
                    anim_changed(clean, true);
                }
                else
                {
                    m_anim_selected = clean;
                }
                m_anim_new_name[0] = 0;
            }
        }
        tooltip(tr("New animations start with a working blink template (random delay, close, open, loop)."));

        Animation* animation = selected_animation();
        if (animation != nullptr)
        {
            ImGui::Separator();
            bool enabled = animation->enabled;
            if (sk.checkbox(tr("Enabled"), &enabled))
            {
                animation->enabled = enabled;
                anim_changed(animation->name, false);
            }
            ImGui::SameLine();
            const bool playing = Registry::instance().anim_playing();
            if (sk.button(playing ? tr("Pause") : tr("Play")))
            {
                Registry::instance().set_anim_playing(!playing);
            }
            ImGui::SameLine();
            if (sk.button(tr("Restart")))
            {
                Registry::instance().restart_animations();
            }
            tooltip(tr("Restarts every animation from its first node."));
            ImGui::SameLine();
            const bool dirty_here = m_anim_dirty.count(animation->name) != 0;
            ImGui::BeginDisabled(!dirty_here);
            if (sk.button(tr("Save")))
            {
                std::string message;
                if (anim_save(m_shelf->folder(), *animation, message))
                {
                    m_anim_dirty.erase(animation->name);
                    m_anim_message = message;
                }
                else
                {
                    m_anim_message = message;
                }
            }
            ImGui::EndDisabled();
            tooltip(tr("Writes this animation to Animations\\<name>.json."));
            ImGui::SameLine();
            if (sk.button(tr("Load")))
            {
                if (dirty_here)
                {
                    ImGui::OpenPopup(tr("Discard unsaved changes?"));
                }
                else
                {
                    load_animation_from_disk(*animation);
                }
            }
            tooltip(tr("Reloads this animation from its file, dropping unsaved changes."));
            if (ImGui::BeginPopupModal(tr("Discard unsaved changes?"), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::TextUnformatted(tr("This animation has unsaved changes."));
                if (sk.button(tr("Load")))
                {
                    load_animation_from_disk(*animation);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (sk.button(tr("Cancel")))
                {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            if (dirty_here)
            {
                ImGui::SameLine();
                ImGui::TextColored(warn(), tr("Unsaved changes"));
            }
            ImGui::SameLine();
            if (sk.button(tr("Delete animation")))
            {
                ImGui::OpenPopup(tr("Delete animation?"));
            }
            if (ImGui::BeginPopupModal(tr("Delete animation?"), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::Text(tr("Delete '%s' from disk?"), animation->name.c_str());
                if (sk.button(tr("Delete")))
                {
                    std::string message;
                    anim_remove(m_shelf->folder(), animation->name, message);
                    const std::string removed = animation->name;
                    m_anims.erase(std::remove_if(m_anims.begin(), m_anims.end(),
                                                 [&removed](const Animation& item) { return item.name == removed; }),
                                  m_anims.end());
                    m_anim_selected = m_anims.empty() ? std::string() : m_anims.front().name;
                    m_anim_dirty.erase(removed);
                    Registry::instance().set_animations(m_anims);
                    Registry::instance().restart_animations();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (sk.button(tr("Cancel")))
                {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            // 上面可能刚把这条动画删掉了，重新取一次指针，避免用到野指针。
            animation = selected_animation();
            if (animation == nullptr)
            {
                return;
            }
            ImGui::Separator();

            // 实时进度（每帧从 Registry 取一次）
            m_anim_live = Registry::instance().anim_live();
            for (size_t t = 0; t < animation->tracks.size(); ++t)
            {
                anim_draw_track(*animation, t);
            }
            if (sk.button(tr("Add track")))
            {
                AnimTrack track;
                AnimNode wait;
                wait.kind = AnimNodeKind::Delay;
                wait.random = true;
                wait.wait_min = 2.0;
                wait.wait_max = 5.0;
                AnimNode close;
                close.kind = AnimNodeKind::Move;
                close.duration = 0.07;
                close.ease = Ease::Out;
                close.target.turn[2] = -14.0;
                AnimNode open;
                open.kind = AnimNodeKind::Move;
                open.duration = 0.14;
                open.ease = Ease::In;
                track.nodes = {wait, close, open};
                animation->tracks.push_back(track);
                anim_changed(animation->name, true);
            }
        }
        else
        {
            ImGui::Spacing();
            ImGui::TextColored(muted(), tr("Create an animation to start - the template is a working blink."));
        }

        // 不做自动保存：改完由「保存」按钮写盘，「加载」按钮从文件重新读。
        if (!m_anim_message.empty())
        {
            ImGui::Spacing();
            ImGui::TextColored(muted(), tr("%s"), m_anim_message.c_str());
        }
    }

    void PanelView::draw_presets()
    {
        Skin& sk = skin();
        if (!m_shelf || !m_shelf->ready())
        {
            ImGui::TextColored(warn(), tr("Preset folder is not set up yet."));
            return;
        }
        if (Clock::now() - m_presets_time > std::chrono::seconds(3))
        {
            refresh_presets();
        }

        ImGui::SeparatorText(tr("Save current sliders"));
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - sk.button_width(tr("Save"), "save") - ImGui::GetStyle().ItemSpacing.x);
        const bool enter =
            ImGui::InputTextWithHint("##name", tr("Preset name"), m_preset_name, sizeof(m_preset_name), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        const std::string clean = PresetShelf::clean_name(m_preset_name);
        ImGui::BeginDisabled(clean.empty());
        if (sk.button(tr("Save"), "save") || (enter && !clean.empty()))
        {
            if (m_shelf->exists(clean))
            {
                m_pending = clean;
                ImGui::OpenPopup(tr("Overwrite preset?"));
            }
            else
            {
                save_preset(clean);
            }
        }
        ImGui::EndDisabled();

        if (ImGui::BeginPopupModal(tr("Overwrite preset?"), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text(tr("'%s' already exists. Replace it?"), m_pending.c_str());
            if (sk.button(tr("Replace")))
            {
                save_preset(m_pending);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (sk.button(tr("Cancel")))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::SeparatorText(tr("Saved presets"));
        const float list_h = std::max(120.0f, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 4.5f);
        sk.begin_child("##presetlist", ImVec2(0, list_h), ImGuiChildFlags_Borders);
        if (m_presets.empty())
        {
            ImGui::TextColored(muted(), tr("No presets yet."));
        }
        for (const std::string& name : m_presets)
        {
            if (ImGui::Selectable(name.c_str(), m_picked == name, ImGuiSelectableFlags_AllowDoubleClick))
            {
                m_picked = name;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    load_preset(name);
                }
            }
        }
        sk.end_child();

        const bool picked = !m_picked.empty() && std::find(m_presets.begin(), m_presets.end(), m_picked) != m_presets.end();
        ImGui::BeginDisabled(!picked);
        if (sk.button(tr("Load"), "load"))
        {
            load_preset(m_picked);
        }
        ImGui::SameLine();
        if (sk.button(tr("Delete"), "delete"))
        {
            m_pending = m_picked;
            ImGui::OpenPopup(tr("Delete preset?"));
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (sk.button(tr("Open folder"), "folder"))
        {
            m_host->open_folder(m_shelf->folder());
        }
        ImGui::SameLine();
        if (sk.button(tr("Reset all"), "reset"))
        {
            ImGui::OpenPopup(tr("Reset all sliders?"));
        }

        if (ImGui::BeginPopupModal(tr("Delete preset?"), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text(tr("Delete '%s' from disk?"), m_pending.c_str());
            if (sk.button(tr("Delete")))
            {
                std::string message;
                m_shelf->remove(m_pending, message);
                set_message(message);
                m_picked.clear();
                refresh_presets();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (sk.button(tr("Cancel")))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopupModal(tr("Reset all sliders?"), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            const std::string who = m_target.label.empty() ? std::string(tr("this character")) : m_target.label;
            ImGui::Text(tr(m_morph_names.empty() ? "Set every bone of %s back to 1.0x?" : "Set every bone of %s back to 1.0x and give every morph back to the game?"),
                        who.c_str());
            if (m_edited.size() > (is_edited(m_target.key) ? 1u : 0u))
            {
                ImGui::TextColored(muted(), tr("Other characters keep their sliders (release them in the Status tab)."));
            }
            if (sk.button(tr("Reset")))
            {
                Registry::instance().clear_edits();
                Registry::instance().clear_morphs();
                set_message(tr("All sliders reset"));
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (sk.button(tr("Cancel")))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(muted(), tr("Folder: %s"), path_to_utf8(m_shelf->folder()).c_str());
        ImGui::PopTextWrapPos();
    }

    void PanelView::draw_skin_picker()
    {
        Skin& sk = skin();
        ImGui::SeparatorText(tr("Window skin"));
        if (Clock::now() - m_skins_time > std::chrono::seconds(3))
        {
            m_skins = sk.available();
            m_skins_time = Clock::now();
        }
        const float open_w = sk.button_width(tr("Open folder"), "folder");
        const float reload_w = sk.button_width(tr("Reload"), "refresh");
        ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x - open_w - reload_w - ImGui::GetStyle().ItemSpacing.x * 2));
        const std::string current = sk.name().empty() ? std::string(Skin::kBuiltinName) + tr(" (built-in)") : sk.display_name();
        if (ImGui::BeginCombo("##skin", current.c_str(), ImGuiComboFlags_HeightLarge))
        {
            if (ImGui::Selectable(tr("Default (built-in)"), sk.name().empty()))
            {
                Skin::request("");
            }
            for (const std::string& n : m_skins)
            {
                if (ImGui::Selectable(n.c_str(), n == sk.name()))
                {
                    Skin::request(n);
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemClicked())
        {
            m_skins = sk.available();
        }
        tooltip(tr("Skins live in the folder next to the presets folder: one sub-folder per skin with a skin.json\n"
                "and PNG/JPG images. See README.txt there. The active skin reloads when its files change."));
        ImGui::SameLine();
        if (sk.button(tr("Reload"), "refresh"))
        {
            Skin::request(sk.name());
            m_skins = sk.available();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(sk.folder().empty());
        if (sk.button(tr("Open folder##skins"), "folder"))
        {
            m_host->open_folder(sk.folder());
        }
        ImGui::EndDisabled();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(muted(), tr("Skin: %s"), sk.status().c_str());
        for (const std::string& p : sk.problems())
        {
            ImGui::TextColored(warn(), tr("  %s"), p.c_str());
        }
        ImGui::PopTextWrapPos();
    }

    void PanelView::draw_status()
    {
        Skin& sk = skin();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(m_host->hook_active() ? good() : warn(), tr("%s"), m_host->hook_text().c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();

        if (sk.button(tr("Rescan"), "refresh"))
        {
            if (m_targets)
            {
                m_targets->request_rescan();
            }
            else
            {
                Registry::instance().request_rescan();
            }
            set_message(tr("Rescan requested"));
        }
        ImGui::SameLine();
        if (m_targets && sk.button(tr("Write diagnostics")))
        {
            set_message(tr("Diagnostics written to " )+ m_targets->write_diagnostics());
        }
        tooltip(tr("Writes what the hook finder sees into the log file next to the DLL.\nSend that file along with bug reports."));
        ImGui::SameLine();
        if (sk.checkbox(tr("Keep this window on top"), &m_topmost))
        {
            m_host->set_topmost(m_topmost);
        }

        ImGui::Spacing();
        ImGui::TextUnformatted(tr("Language"));
        ImGui::SameLine();
        {
            int current = static_cast<int>(language());
            const char* names[2] = {"English", "\xE4\xB8\xAD\xE6\x96\x87"};
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::Combo("##language", &current, names, 2))
            {
                set_language(static_cast<Language>(current));
            }
        }
        tooltip(tr("English / \xE4\xB8\xAD\xE6\x96\x87. The default comes from Language in config.lua (auto = system language)."));

        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        if (m_targets)
        {
            ImGui::TextColored(muted(), tr("Lua link: %s"), m_targets->link_text().c_str());
        }
        if (Registry::instance().cinematic())
        {
            ImGui::TextColored(accent(), tr("Cutscene: %s"), tr("in a cutscene - the locomotion layer stays on"));
        }
        else
        {
            ImGui::TextColored(muted(), tr("Cutscene: %s"), tr("no"));
        }
        ImGui::TextColored(muted(), tr("Mirroring: %s"), m_mirror_text.c_str());
        ImGui::TextColored(muted(), tr("Simplified Panel: %zu group(s)%s; %s"), m_body.groups.size(),
                           m_body.style.empty() ? "" : (", " + m_body.style + tr(" rig")).c_str(), m_body_status.c_str());
        ImGui::TextColored(muted(), tr("Morphs: %zu on the meshes%s, %zu set; game profile: %s"), m_morph_names.size(),
                           m_morphs_hidden ? (" (+" + std::to_string(m_morphs_hidden) + tr(" hidden by ExcludeMorphs)")).c_str() : "", m_morphs.size(),
                           m_morph_status.c_str());
        const std::string renderer = m_host->renderer_text();
        if (!renderer.empty())
        {
            ImGui::TextColored(muted(), tr("Window drawing: %s"), renderer.c_str());
        }
        ImGui::PopTextWrapPos();

        draw_edited_characters();
        draw_skin_picker();

        ImGui::SeparatorText(tr("Tracked meshes"));
        if (ImGui::BeginTable("##rigs", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn(tr("Component"));
            ImGui::TableSetupColumn(tr("Bones"));
            ImGui::TableSetupColumn(tr("Frames seen"));
            ImGui::TableHeadersRow();
            for (const auto& r : m_rigs)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextColored(r.stale ? warn() : r.active ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : muted(), tr("%s / %s%s%s"), r.owner.c_str(),
                                   r.label.c_str(), r.primary && r.active ? tr(" (bones listed)") : "", r.stale ? tr(" (stale)") : "");
                if (!r.owner.empty() && ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(tr("%s"), r.owner.c_str());
                }
                ImGui::TableNextColumn();
                ImGui::Text(tr("%d"), r.bones);
                ImGui::TableNextColumn();
                ImGui::Text(tr("%llu"), static_cast<unsigned long long>(r.frames));
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        const std::string key = m_targets ? m_targets->hotkey_text() : std::string(tr("F6"));
        ImGui::TextColored(muted(), tr("%s opens/closes this window (in game or here). Changes are visual only: collision, "
                                    "physics bodies and sockets keep their original size. While the game is paused the "
                                    "pose updates on the next animated frame."),
                           key.c_str());
        ImGui::PopTextWrapPos();
    }
} // namespace uuepbs::ui
