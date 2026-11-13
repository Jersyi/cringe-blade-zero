#include "npc_bridge.h"

#include "npc_body.h"

#include "panel_view.hpp"
#include "anim.hpp"
#include "presets.hpp"
#include "registry.hpp"
#include "slider_window.hpp"
#include "settings_store.h"
#include <Windows.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace NpcBridge
{
    namespace
    {
        // 界面只认一个角色：主角。选择器固定一项，切换按钮什么都不做 —— 我们不放 NPC 编辑。
        class Target final : public uuepbs::ui::TargetSource
        {
          public:
            std::vector<uuepbs::ui::TargetChoice> targets() override { return {{"player", "主角", "player"}}; }
            uuepbs::ui::TargetChoice current_target() override { return {"player", "主角", "player"}; }
            void pick_target(const std::string&) override {}
            void refresh_targets() override {}
            void relist_targets() override {}
            void request_rescan() override {}
            std::string refresh_hotkey_text() override { return std::string(); }
            std::string link_text() override
            {
                return std::string("豪之刃内置；骨骼钩子") + (NpcBody::Hooked() ? "已装" : "未装");
            }
            std::string hotkey_text() override { return std::string("F11（快捷键页可改）/ F10 → 身材 / 骨骼"); }
            std::string write_diagnostics() override { return std::string(); }
        };

        Target g_target;
        uuepbs::PresetShelf g_shelf;
        std::atomic<bool> g_started{false};  // read by the hub, hotkey and game threads
        // Locomotion 的当前档位：v1.3 原本由 Lua 每 150ms 送一次，我们在这里自己记。
        std::string g_loco_context = "unarmed";
        int g_loco_state = -1;
        void* g_fed = nullptr;
        int32_t g_fed_bones = 0;
        // [NpcBody] Enabled (default 0: without it the hero's body is never touched). Opening the editor turns it on.
        volatile LONG g_enabled = -1;  // -1: not read yet
        SRWLOCK g_start_lock = SRWLOCK_INIT;  // Start runs on the game thread (hidden) and on the hub / hotkey threads

        std::string Narrow(const wchar_t* text)
        {
            const int need = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
            if (need <= 1)
            {
                return std::string();
            }
            std::string out(static_cast<size_t>(need), '\0');
            ::WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), need, nullptr, nullptr);
            out.resize(static_cast<size_t>(need) - 1);
            return out;
        }

        std::filesystem::path DataFolder(const wchar_t* leaf)
        {
            // This DLL's own folder (the game also loads the system version.dll: never look it up by name).
            wchar_t path[MAX_PATH * 2]{};
            HMODULE self = nullptr;
            ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 reinterpret_cast<LPCWSTR>(&DataFolder), &self);
            const DWORD got = self ? ::GetModuleFileNameW(self, path, _countof(path)) : 0;
            std::filesystem::path out;
            if (got > 0)
            {
                out = std::filesystem::path(path).parent_path();
            }
            if (leaf)
            {
                out /= leaf;
            }
            return out;
        }
    } // namespace

    // UUEPBS 从别的线程拿地址来读姿势：走这个函数，读到野地址时返回 false 而不是崩。
    bool SafeRead(uintptr_t address, void* out, size_t size)
    {
        if (!address || !out || !size)
        {
            return false;
        }
        __try
        {
            std::memcpy(out, reinterpret_cast<const void*>(address), size);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool Feed(void* component)
    {
        if (!component || !NpcBody::Ready())
        {
            return false;
        }
        NpcBody::Names(component);
        NpcBody::Parents(component);
        const int32_t bones = NpcBody::SnapshotCount();
        if (bones <= 0)
        {
            return false;
        }

        auto& registry = uuepbs::Registry::instance();
        registry.set_safe_reader(&SafeRead);

        const int32_t buffers = NpcBody::BufferOffset();
        const int32_t read_index = NpcBody::ReadIndexOffset();
        if (buffers > 0 && read_index > 0)
        {
            uuepbs::PoseLayout layout{};
            layout.buffers = static_cast<uint32_t>(buffers);
            layout.read_index = static_cast<uint32_t>(read_index);
            layout.array_stride = 16;
            layout.elem_size = 96;
            registry.set_layout(layout);
        }

        const uintptr_t key = reinterpret_cast<uintptr_t>(component);
        if (registry.is_tracked(key) && g_fed == component && g_fed_bones == bones)
        {
            return true;
        }

        constexpr int32_t kSlots = 4096;
        constexpr int32_t kChars = 96;
        std::vector<wchar_t> text(static_cast<size_t>(kSlots) * static_cast<size_t>(kChars), L'\0');
        const int32_t got = NpcBody::SnapshotNames(text.data(), kChars, kSlots);
        if (got <= 0)
        {
            return false;
        }

        std::vector<std::string> names;
        std::vector<int32_t> parents;
        names.reserve(static_cast<size_t>(got));
        parents.reserve(static_cast<size_t>(got));
        for (int32_t i = 0; i < got; ++i)
        {
            names.push_back(Narrow(text.data() + static_cast<size_t>(i) * static_cast<size_t>(kChars)));
            parents.push_back(NpcBody::ParentOf(i));
        }

        std::string message;
        if (!registry.track(key, "CharacterMesh0", "主角", std::move(names), std::move(parents), true, message))
        {
            return false;
        }
        registry.set_active_actor(uuepbs::kPlayerActor, "主角", "auto");
        g_fed = component;
        g_fed_bones = bones;
        return true;
    }

    void OnPoseFinalized(void* component)
    {
        if (!g_fed || component != g_fed)
        {
            return;
        }
        uuepbs::Registry::instance().on_pose_finalized(component);
    }

    bool Enabled()
    {
        LONG on = InterlockedCompareExchange(&g_enabled, 0, 0);
        if (on < 0)
        {
            on = SettingsStore::GetInt(L"NpcBody", L"Enabled", 0) != 0 ? 1 : 0;
            InterlockedExchange(&g_enabled, on);
        }
        return on != 0;
    }

    void SetEnabled(bool on)
    {
        InterlockedExchange(&g_enabled, on ? 1 : 0);
        SettingsStore::PutInt(L"NpcBody", L"Enabled", on ? 1 : 0);
        if (!on && g_started)
        {
            uuepbs::ui::set_visible(false);
        }
    }

    void Start(bool show)
    {
        AcquireSRWLockExclusive(&g_start_lock);
        if (g_started)
        {
            ReleaseSRWLockExclusive(&g_start_lock);
            if (show)
            {
                uuepbs::ui::set_visible(true);
            }
            return;
        }
        g_shelf.set_folder(DataFolder(L"UUEPBS Presets"));

        // 时间轴动画（眨眼这类）在 v1.3 里只有打开「动画」页时才会被读进 Registry
        // （panel_view.cpp:1901 的 m_anim_loaded）。我们这里在窗口一开就先读一遍，
        // 否则不进那一页就等于没装过动画 —— 这正是「动画不生效」的真正原因。
        {
            std::vector<std::string> errors;
            std::vector<uuepbs::Animation> animations = uuepbs::anim_load_all(g_shelf.folder(), &errors);
            if (!animations.empty())
            {
                uuepbs::Registry::instance().set_animations(std::move(animations));
            }
        }
        // 上次的滑条（_last_session，窗口自动存的那一份）：v1.3 由 Lua 在开局读回，这里没有 Lua，自己读。
        {
            uuepbs::EditBook book;
            uuepbs::MorphBook morphs;
            uuepbs::LocoBookMap loco;
            std::string message;
            if (g_shelf.load(uuepbs::PresetShelf::kSessionName, book, &loco, message, &morphs))
            {
                uuepbs::Registry::instance().replace_edits(std::move(book));
                uuepbs::Registry::instance().replace_morphs(std::move(morphs));
                uuepbs::Registry::instance().replace_loco_all(std::move(loco));
            }
        }

        uuepbs::ui::Options options;
        options.version = "v1.3 in Cringe Blade Zero";
        options.targets = &g_target;
        options.cpu_renderer = true;
        options.topmost = false;
        options.session_autosave = true;
        options.skins_folder = DataFolder(L"UUEPBS Skins");
        if (uuepbs::ui::start(&g_shelf, options))
        {
            g_started = true;
            // start() only creates the window; it is born hidden (m_shown = false in slider_window.cpp:717)
            // and the panel lives in a separate top-level window, so it has to be shown explicitly.
            if (show)
            {
                uuepbs::ui::set_visible(true);
            }
        }
        ReleaseSRWLockExclusive(&g_start_lock);
    }

    void SetLocoSpeed(double centimeters_per_second)
    {
        if (!g_started)
        {
            return;
        }
        int state = 0;
        if (centimeters_per_second >= 700.0)
        {
            state = 3;
        }
        else if (centimeters_per_second >= 300.0)
        {
            state = 2;
        }
        else if (centimeters_per_second >= 30.0)
        {
            state = 1;
        }
        // 只在状态真的变了才写：set_loco_live 会触发一次交叉淡入淡出，每帧都写会一直重来。
        if (state == g_loco_state)
        {
            return;
        }
        g_loco_state = state;
        uuepbs::Registry& registry = uuepbs::Registry::instance();
        registry.set_loco_live(g_loco_context, state, true);
    }

    void SetLocoContext(const char* context)
    {
        if (!context || !*context)
        {
            return;
        }
        if (g_loco_context == context)
        {
            return;
        }
        g_loco_context = context;
        if (!g_started)
        {
            return;
        }
        uuepbs::Registry& registry = uuepbs::Registry::instance();
        registry.remember_loco_context(g_loco_context);
        // 换武器档要立刻让新档接上，所以把当前状态重报一次。
        if (g_loco_state >= 0)
        {
            registry.set_loco_live(g_loco_context, g_loco_state, true);
        }
    }

    void Stop()
    {
        if (!g_started)
        {
            return;
        }
        uuepbs::ui::stop();
        g_started = false;
    }

    void Toggle()
    {
        if (!Enabled())
        {
            SetEnabled(true);  // opening the editor is turning 身材 / 骨骼 on
        }
        if (!g_started)
        {
            Start(true);
            return;
        }
        uuepbs::ui::toggle();
    }

    bool Visible() { return g_started && uuepbs::ui::visible(); }

    bool Started() { return g_started; }

    // ---- 预设（插件接口用，任意线程）：和面板的「载入 / 存」同一份文件、同一套 Registry ----
    namespace
    {
        // The shelf's folder is set by Start; the API may ask before the editor ever started.
        void EnsureShelf()
        {
            AcquireSRWLockExclusive(&g_start_lock);
            if (!g_shelf.ready())
            {
                g_shelf.set_folder(DataFolder(L"UUEPBS Presets"));
            }
            ReleaseSRWLockExclusive(&g_start_lock);
        }
    } // namespace

    std::vector<std::string> PresetNames()
    {
        EnsureShelf();
        return g_shelf.list();
    }

    bool PresetLoad(const std::string& name, std::string& message)
    {
        EnsureShelf();
        uuepbs::EditBook book;
        uuepbs::MorphBook morphs;
        uuepbs::LocoBookMap loco;
        if (!g_shelf.load(name, book, &loco, message, &morphs))
        {
            return false;
        }
        uuepbs::Registry::instance().replace_edits(std::move(book));
        uuepbs::Registry::instance().replace_morphs(std::move(morphs));
        uuepbs::Registry::instance().replace_loco_all(std::move(loco));
        return true;
    }

    bool PresetSave(const std::string& name, std::string& message)
    {
        EnsureShelf();
        const std::string clean = uuepbs::PresetShelf::clean_name(name);
        if (clean.empty() || clean == uuepbs::PresetShelf::kSessionName)
        {
            message = "preset name not allowed";
            return false;
        }
        const uuepbs::MorphBook morphs = uuepbs::Registry::instance().morphs();
        return g_shelf.save(clean, uuepbs::Registry::instance().edits(), uuepbs::Registry::instance().loco_all(), message, &morphs);
    }

    bool PresetRemove(const std::string& name, std::string& message)
    {
        EnsureShelf();
        if (name.empty() || name == uuepbs::PresetShelf::kSessionName)
        {
            message = "preset name not allowed";
            return false;
        }
        return g_shelf.remove(name, message);
    }
} // namespace NpcBridge
