#include "i18n.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <atomic>
#include <unordered_map>

namespace uuepbs::ui
{
    namespace
    {
        std::atomic<int> g_language{static_cast<int>(Language::Chinese)};

        bool system_is_chinese()
        {
            const LANGID id = GetUserDefaultUILanguage();
            return PRIMARYLANGID(id) == LANG_CHINESE;
        }

        // Main interface strings. Format specifiers must be kept exactly as in the English text.
        const std::unordered_map<std::string_view, const char*> kChinese = {
            // tabs
            {"Simplified Panel###simplified", "简化面板###simplified"},
            {"Detailed Panel###detailed", "详细面板###detailed"},
            {"Locomotion###locomotion", "移动状态###locomotion"},
            {"Presets###presets", "预设###presets"},
            {"Status###status", "状态###status"},
            // header / target picker
            {"Enabled", "启用"},
            {"Character", "角色"},
            {"Refresh", "刷新"},
            {"Release", "释放"},
            {"Select", "选择"},
            {"Rescan", "重新扫描"},
            {"No character found yet. Pick one above or use Status > Rescan.", "还没有找到角色。请在上方选择，或用「状态 > 重新扫描」。"},
            {"Found %s - waiting for the pose hook (see Status)", "已找到 %s — 等待姿态钩子（见状态页）"},
            {"Tracking %zu mesh%s - waiting for an animated frame", "正在跟踪 %zu 个网格%s — 等待动画帧"},
            {"Live on %zu mesh%s", "已在 %zu 个网格%s 上生效"},
            {"- %s, %zu bones", "- %s，%zu 根骨骼"},
            {"- %s, %zu bones, %zu morphs", "- %s，%zu 根骨骼，%zu 个形变目标"},
            {"Also keeping sliders on: %s", "同时保留滑条的角色：%s"},
            {"Remembered NPCs (%zu): %s", "已记住的 NPC（%zu）：%s"},
            {"None yet. A character gets its own sliders once you pick it and move one.", "还没有。选中角色并拖动任意滑条后，它才会拥有自己的数据。"},
            {"- remembered", "- 已记住"},
            {"- this visit only", "- 仅本次"},
            {"The player's sliders are saved with the session (_last_session).", "玩家的滑条随会话保存（_last_session）。"},
            {"%s (selected)", "%s（已选中）"},
            // presets
            {"Preset name", "预设名称"},
            {"Save", "保存"},
            {"Load", "加载"},
            {"Delete", "删除"},
            {"Replace", "替换"},
            {"Cancel", "取消"},
            {"Open folder", "打开文件夹"},
            {"No presets yet.", "还没有预设。"},
            {"Preset folder is not set up yet.", "预设文件夹尚未就绪。"},
            {"Folder: %s", "文件夹：%s"},
            {"'%s' already exists. Replace it?", "「%s」已存在，要覆盖吗？"},
            {"Delete '%s' from disk?", "要从磁盘删除「%s」吗？"},
            // status
            {"Lua link: %s", "Lua 连接：%s"},
            {"Mirroring: %s", "镜像：%s"},
            {"Skin: %s", "皮肤：%s"},
            {"Default (built-in)", "默认（内置）"},
            {"Keep this window on top", "窗口置顶"},
            {"Window drawing: %s", "窗口渲染：%s"},
            {"Write diagnostics", "写入诊断"},
            {"Reload", "重新加载"},
            {"Reset", "重置"},
            {"Reset all", "全部重置"},
            {"Reset all morphs", "重置全部形变"},
            {"Language", "语言"},
            // simplified / detailed editor
            {"Search bones...", "搜索骨骼..."},
            {"Search morphs...", "搜索形变..."},
            {"Edited only", "只看已编辑"},
            {"Pick a bone above to edit it.", "在上方选一根骨骼开始编辑。"},
            {"No bones yet - no character mesh has been found.", "还没有骨骼——尚未找到角色网格。"},
            {"Edit length / width / depth separately", "分开编辑长/宽/深"},
            {"Reset bone", "重置骨骼"},
            {"Reset scale", "重置缩放"},
            {"Reset rotation", "重置旋转"},
            {"Reset move", "重置位移"},
            {"Mirror to other side", "镜像到另一侧"},
            {"Mirror left/right morphs", "左右镜像形变"},
            {"Ctrl+click to type a value. Ctrl+wheel nudges (Shift: finer).", "Ctrl+点击可输入数值，Ctrl+滚轮微调（Shift 更精细）。"},
            {"Ctrl+click a slider to type a value. Ctrl+wheel nudges it (add Shift for finer steps).",
             "Ctrl+点击滑条可输入数值，Ctrl+滚轮微调（加 Shift 更精细）。"},
            {"Give these morphs back to the game (its own value comes back).", "把这些形变还给游戏（恢复原值）。"},
            {"XYZ", "XYZ"},
            // locomotion tab
            {"Now: %s", "当前：%s"},
            {"· %s · layer %s", "· %s · 叠加层 %s"},
            {"Weapon context:", "武器类型："},
            {"Unarmed (sheathed)", "空手（武器收在背上）"},
            {"Cutscene (story scene)", "过场（剧情演出）"},
            {"Editing:", "编辑："},
            {"Clear this state", "清空该状态"},
            {"Preview this context/state", "预览当前类型/状态"},
            {"preview forcing this context on", "预览中：强制应用当前类型"},
            {"preview off", "预览关闭"},
            {"applies to context/state changes", "作用于类型/状态切换"},
            {"Blend (ms)", "过渡（毫秒）"},
            {"Removes every offset of the selected context + state. Everything else keeps its values.",
             "清空当前「武器类型 + 状态」的全部偏移，其它组合保留。"},
            {"Unarmed = weapon on the back. The other entries are weapon types you have drawn once;\neach one keeps its own Idle/Walk/Run/Sprint offsets.\nCutscene = used while a story sequence plays; its state is pinned to Idle.",
             "空手 = 武器收在背上。其它条目是你拔出过的武器类型；\n每种武器都有自己的 待机/走/跑/冲刺 偏移。\n过场 = 剧情演出时使用的一档，状态固定为「待机」。"},
            {"Cross-fade time when the context (unarmed / weapon), the state or the on/off flag changes.\n0 = snap instantly. The default comes from LocomotionBlendMs in config.lua.",
             "切换武器类型、移动状态或叠层开关时的过渡时间。\n0 = 瞬间切换。默认值来自 config.lua 的 LocomotionBlendMs。"},
            {"Applies the selected context/state while you stand still, so the sliders can be tuned\nwithout running or switching weapons. Only active while this tab is open.",
             "站着不动也能应用当前类型/状态，方便调数值，不用边跑边调或频繁换武器。\n仅在打开本页时有效。"},
            // ---- 第二轮补齐：界面全中文（2026-11-07）----
            {"Idle", "待机"},
            {"Walk", "行走"},
            {"Run", "奔跑"},
            {"Sprint", "冲刺"},
            {"on", "开"},
            {"off", "关"},
            {"unknown", "未知"},
            {"Length (X)", "长度 (X)"},
            {"Width (Y)", "宽度 (Y)"},
            {"Depth (Z)", "深度 (Z)"},
            {"Roll (X)", "翻滚 (X)"},
            {"Pitch (Y)", "俯仰 (Y)"},
            {"Yaw (Z)", "偏航 (Z)"},
            {"Along (X)", "前后 (X)"},
            {"Side (Y)", "左右 (Y)"},
            {"Up (Z)", "上下 (Z)"},
            {"%.2f cm", "%.2f 厘米"},
            {"Scale###scale", "缩放###scale"},
            {"Scale  *###scale", "缩放  *###scale"},
            {"Rotate###rotate", "旋转###rotate"},
            {"Rotate  *###rotate", "旋转  *###rotate"},
            {"Move###move", "位移###move"},
            {"Move  *###move", "位移  *###move"},
            {"  rot", "  旋转"},
            {"  move", "  位移"},
            {"Children", "子级"},
            {"Scale children too", "子级一起缩放"},
            {"Keep children size", "保持子级尺寸"},
            {"This bone only", "只改这根骨骼"},
            {"Scale children too: children follow the bone's scale, rotation and move.\nKeep children size: children follow rotation/move and the new shape but keep their own size.\nThis bone only: nothing below it changes (rotation/move included).", "子级一起缩放：子级跟随这根骨骼的缩放、旋转和位移。\n保持子级尺寸：子级跟随旋转/位移和新的形状，但保持自己的尺寸。\n只改这根骨骼：它下面的所有东西都不变（包括旋转/位移）。"},
            {"Bones", "骨骼"},
            {"Morphs", "形变"},
            {"Morphs:", "形变："},
            {"\nBones:", "\n骨骼："},
            {"\nMorphs:", "\n形变："},
            {"Bones (%zu)###bones", "骨骼 (%zu)###bones"},
            {"Morphs (%zu)###morphs", "形变 (%zu)###morphs"},
            {"Component", "组件"},
            {"Upper arms", "上臂"},
            {"(searching...)", "（搜索中…）"},
            {"Looking for characters...", "正在查找角色…"},
            {"Automatic (player / game default)", "自动（玩家 / 游戏默认）"},
            {"Switching to ", "正在切换到 "},
            {"Switching to the default character", "正在切换到默认角色"},
            {"   - edited", "   - 已编辑"},
            {"Edited characters", "已编辑的角色"},
            {"this character", "此角色"},
            {"Whose body the sliders edit. The list shows characters currently loaded in the level, nearest first,\nwith their distance from the player. Characters you have edited are always listed.\nCharacters you have edited keep their sliders while you edit someone else (marked \"edited\").\nCharacters you never edited are left alone.", "滑条作用在谁身上。列表显示当前关卡里已加载的角色，按离玩家的距离由近到远排列。\n你编辑过的角色总会列出。\n编辑过的角色在你编辑别人时会保留自己的滑条（标记为「已编辑」）。\n没编辑过的角色不会被改动。"},
            {"Rescan the character and its meshes, relist the characters and retry the pose hook.", "重新扫描角色和它的网格、重新列出角色，并重试姿态钩子。"},
            {"\nHotkey: ", "\n快捷键："},
            {" (works in game too).", "（游戏中同样有效）。"},
            {"These characters keep their own sliders while you edit this one.\nPick one above to change it, or release it in the Status tab (Edited characters).", "这些角色在你编辑当前角色时会保留自己的滑条。\n在上方选一个可以切换，或在「状态」页的「已编辑的角色」里释放。"},
            {"Their sliders come back automatically when they are loaded within CharacterRange of the player.\nFiles: _characters folder inside the presets folder.", "当它们在玩家的 CharacterRange 范围内加载时，滑条会自动恢复。\n文件：预设文件夹里的 _characters 文件夹。"},
            {"Remembered as \"", "记住为「"},
            {"\": these sliders come back by themselves whenever this character\nis loaded near the player (after a reload, in a new area...). Release forgets it.", "」：这些滑条会在该角色\n加载到玩家附近时自动恢复（重载后、进入新区域等）。「释放」会忘记它。"},
            {"This character has no stable name to recognise it by after a reload, so its sliders last until it despawns.\nSave a preset to reuse them.", "这个角色没有稳定名字，重载后无法识别，所以它的滑条只在本局有效。\n保存预设可以留着重用。"},
            {"Drop this character's sliders. Its body goes back to normal, and it is left alone from now on\n(unless you pick it and edit it again).", "丢弃这个角色的滑条。它的身体恢复正常，之后不再改动它\n（除非你再次选中并编辑它）。"},
            {"Sliders released", "滑条已释放"},
            {"The player's sliders are saved with the session. NPCs are remembered by name and get their sliders back whenever they are loaded near the player; Release or Reset all forgets them.", "玩家的滑条随会话保存。NPC 按名字记住，加载到玩家附近时会恢复它们的滑条；「释放」或「全部重置」会忘记它们。"},
            {"The sliders appear once a character's skeletal mesh has been found. Load a save or enter a level, or pick a character above.", "找到角色的骨骼网格后滑条才会出现。读取存档或进入关卡，或在上方选择一个角色。"},
            {"This skeleton uses bone names the Simplified Panel does not recognise. Every bone can still be edited in the Detailed Panel.", "这个骨架的骨骼名「简化面板」不认识，但所有骨骼仍可在「详细面板」里编辑。"},
            {"%zu morph target%s: see Detailed Panel > Morphs. Add \"MorphGroups\" to the game profile to put sliders here.", "形变目标 %zu 个%s：见「详细面板 > 形变」。在游戏配置里加上 \"MorphGroups\" 就能在这里放滑条。"},
            {"The game keeps setting this morph itself (animation or its own code),\nso the slider may not stick. UUEPBS re-applies it each time it notices.", "游戏自己在不断设置这个形变（动画或它自己的代码），\n所以滑条可能压不住。UUEPBS 每次发现被改回都会重新应用。"},
            {"Moving a _L morph also moves its _R twin (and the other way round).", "拖动 _L 形变时对应的 _R 也会跟着动（反之亦然）。"},
            {"A set morph replaces the game's value (even at 0); Reset gives it back. Ctrl+click to type values outside 0..1.", "设置过的形变会覆盖游戏的数值（即使是 0）；「重置」把值还给游戏。Ctrl+点击可输入超出 0~1 的数值。"},
            {"All morphs given back to the game", "所有形变已还给游戏"},
            {"Bone offsets per locomotion state. They are added on top of the Detailed Panel edits and apply only while the layer is on: weapon on the back (unarmed) or drawn (one context per weapon type), and no action playing.\nStory sequences use their own Cutscene entry (state pinned to Idle).", "按移动状态分别保存的骨骼偏移。它叠加在「详细面板」的编辑之上，只在叠层开启时生效：武器收在背上（空手）或拿在手里（按动作模组分档），并且没有动作在播放。\n剧情演出另有一档「过场」，状态固定为「待机」。"},
            {"Save current sliders", "保存当前滑条"},
            {"Saved presets", "已保存的预设"},
            {"Overwrite preset?", "覆盖预设？"},
            {"Delete preset?", "删除预设？"},
            {"Reset all sliders?", "重置全部滑条？"},
            {"Set every bone of %s back to 1.0x?", "把 %s 的每根骨骼都设回 1.0x？"},
            {"Set every bone of %s back to 1.0x and give every morph back to the game?", "把 %s 的每根骨骼都设回 1.0x，并把所有形变还给游戏？"},
            {"Other characters keep their sliders (release them in the Status tab).", "其它角色的滑条会保留（可在「状态」页释放）。"},
            {"All sliders reset", "全部滑条已重置"},
            {"Window skin", "窗口皮肤"},
            {"Window skin: ", "窗口皮肤："},
            {" (built-in)", "（内置）"},
            {"Open folder##skins", "打开文件夹##skins"},
            {"Skins live in the folder next to the presets folder: one sub-folder per skin with a skin.json\nand PNG/JPG images. See README.txt there. The active skin reloads when its files change.", "皮肤放在预设文件夹旁边的文件夹里：每个皮肤一个子文件夹，里面放 skin.json\n和 PNG/JPG 图片。具体见那里的 README.txt。当前皮肤的文件变化时会自动重载。"},
            {"Rescan requested", "已请求重新扫描"},
            {"Diagnostics written to ", "诊断已写入 "},
            {"Writes what the hook finder sees into the log file next to the DLL.\nSend that file along with bug reports.", "把钩子查找过程写到 DLL 旁边的日志文件里。\n反馈问题时请一并附上该文件。"},
            {"English / 中文. The default comes from Language in config.lua (auto = system language).", "英文 / 中文。默认值来自 config.lua 的 Language（auto = 跟随系统语言）。"},
            {"Simplified Panel: %zu group(s)%s; %s", "简化面板：%zu 组%s；%s"},
            {"Morphs: %zu on the meshes%s, %zu set; game profile: %s", "形变：网格上 %zu 个%s，已设置 %zu 个；游戏配置：%s"},
            {" rig", " 绑定"},
            {" hidden by ExcludeMorphs)", " 个被 ExcludeMorphs 隐藏）"},
            {"Tracked meshes", "已跟踪网格"},
            {"Frames seen", "已见帧数"},
            {" (bones listed)", "（已列出骨骼）"},
            {" (stale)", "（已过期）"},
            {"%s opens/closes this window (in game or here). Changes are visual only: collision, physics bodies and sockets keep their original size. While the game is paused the pose updates on the next animated frame.", "%s 开关本窗口（游戏内或窗口内都行）。改动只影响外观：碰撞、物理体和挂点保持原始尺寸。游戏暂停时，姿势会在下一个动画帧更新。"},
            {"by XTGMods", "作者：XTGMods"},
            {"UUEPBS - Universal Unreal Engine Player Body Sliders", "UUEPBS - 通用虚幻引擎玩家体型滑条"},
            {"CPU (GPU window could not be created)", "CPU（无法创建 GPU 窗口）"},
            {"CPU (no Direct3D in use)", "CPU（未使用 Direct3D）"},
            {"GPU (Direct3D 11)", "GPU（Direct3D 11）"},
            {"waiting for Scripts/main.lua (is the mod enabled in UE4SS?)", "正在等待 Scripts/main.lua（mod 在 UE4SS 里启用了吗？）"},
            {"connected, script ", "已连接，脚本 "},
            {" mesh(es)", " 个网格"},
            {", game ", "，游戏 "},
            {", UE ", "，UE "},
            {"could not measure from ", "无法从 "},
            {" (no left/right bone pairs found)", " 测量（没找到左右对称骨骼对）"},
            {"measured from ", "测量自 "},
            {" pairs across ", " 对，对称轴 "},
            {" uncertain", " 个不确定"},
            {"not measured yet", "尚未测量"},
            {"waiting for a pose to measure", "等待一帧姿势来测量"},
            // ---- 时间轴动画面板（2026-11-09）----
            {"Animation###animation", "动画###animation"},
            {"Track %d", "轨道 %d"},
            {"(no bones)", "（未选骨骼）"},
            {"bone(s)", "根骨骼"},
            {"Clear bones", "清空骨骼"},
            {"Select any number of bones.", "可以选任意多根骨骼。"},
            {"Loop", "循环"},
            {"Remove track", "删除轨道"},
            {"Pick the bones this track drives.", "先给这条轨道选骨骼。"},
            {"Move", "驱动"},
            {"Delay", "延迟"},
            {"Duration", "时长"},
            {"%.2f s", "%.2f 秒"},
            {"Linear", "线性"},
            {"Smooth", "平滑"},
            {"Ease in", "渐入"},
            {"Ease out", "渐出"},
            {"Ease in-out", "渐入渐出"},
            {"Rotation", "旋转"},
            {"Translation", "位移"},
            {"Scale", "缩放"},
            {"Fixed", "固定"},
            {"Random", "随机"},
            {"Wait", "等待"},
            {"Min", "最小"},
            {"Max", "最大"},
            {"Up", "上移"},
            {"Down", "下移"},
            {"+ Move node", "＋驱动节点"},
            {"+ Delay node", "＋延迟节点"},
            {"Timeline animations: pick a group of bones and chain nodes.\nA Move node blends those bones to a target over a set time (linear / smooth / ease in / ease out / ease in-out).\nA Delay node waits a fixed time, or a random time in a range, before the next node.\nTracks loop, so a blink is: random delay -> close -> open -> loop. Only the player character is animated.", "时间轴动画：选一组骨骼，然后把节点串起来。\n驱动节点在设定时间内把这组骨骼过渡到目标值（线性 / 平滑 / 渐入 / 渐出 / 渐入渐出）。\n延迟节点会先等一段固定时间、或某个随机区间里的时间，再执行后面的节点。\n轨道可以循环，所以眨眼就是：随机延迟 → 闭眼 → 睁眼 → 循环。只作用于玩家角色。"},
            {"(none)", "（无）"},
            {"New animation name", "新动画名称"},
            {"New", "新建"},
            {"New animations start with a working blink template (random delay, close, open, loop).", "新动画会带一套能直接用的眨眼模板（随机延迟、闭眼、睁眼、循环）。"},
            {"Pause", "暂停"},
            {"Play", "播放"},
            {"Restart", "重播"},
            {"Restarts every animation from its first node.", "把所有动画都从第一个节点重新开始。"},
            {"Delete animation", "删除动画"},
            {"Delete animation?", "删除动画？"},
            {"Add track", "添加轨道"},
            {"Create an animation to start - the template is a working blink.", "先新建一个动画——模板本身就是一套能用的眨眼。"},
            // ---- 动画保存/加载（2026-11-09）----
            {"Writes this animation to Animations\\<name>.json.", "把当前动画写入 Animations\\<name>.json。"},
            {"Reloads this animation from its file, dropping unsaved changes.", "从文件重新读入当前动画，未保存的修改会丢弃。"},
            {"Discard unsaved changes?", "放弃未保存的修改？"},
            {"This animation has unsaved changes.", "这个动画有还没保存的修改。"},
            {"Unsaved changes", "未保存"},
            // ---- 过场状态（2026-11-09）----
            {"Cutscene: %s", "过场动画：%s"},
            {"in a cutscene - the locomotion layer stays on", "过场中（叠加层保持开启）"},
            {"no", "否"},
            {"· cutscene, layer kept on", "· 过场中，叠加层保持开启"},
        };
    } // namespace

    void set_language(Language language)
    {
        g_language.store(static_cast<int>(language), std::memory_order_relaxed);
    }

    Language language()
    {
        return static_cast<Language>(g_language.load(std::memory_order_relaxed));
    }

    void set_language_from_setting(std::string_view setting)
    {
        if (setting == "auto" || setting.empty())
        {
            set_language(system_is_chinese() ? Language::Chinese : Language::English);
            return;
        }
        if (setting == "zh" || setting == "cn" || setting == "chinese" || setting == "zh-cn")
        {
            set_language(Language::Chinese);
            return;
        }
        set_language(Language::English);
    }

    const char* language_name(Language language)
    {
        switch (language)
        {
        case Language::Chinese:
            return "中文";
        case Language::English:
        default:
            return "English";
        }
    }

    const char* tr(const char* text)
    {
        if (text == nullptr)
        {
            return "";
        }
        if (g_language.load(std::memory_order_relaxed) != static_cast<int>(Language::Chinese))
        {
            return text;
        }
        const auto it = kChinese.find(std::string_view(text));
        return it == kChinese.end() ? text : it->second;
    }
} // namespace uuepbs::ui
