# 项目架构

这份文档讲豪之刃（Cringe Blade Zero）的代码怎么组织、运行时怎么跑、改某类东西要动哪些文件。给第一次接手仓库的人和 AI 助手看。

- 先读仓库根目录的 [AGENTS.md](../AGENTS.md)：硬规则、编译和测试命令。
- 写插件或 Lua 脚本的人看 [AGENT_PLUGIN_GUIDE.md](AGENT_PLUGIN_GUIDE.md) 和 [PLUGIN_API.md](PLUGIN_API.md)，不必读这份。
- 文中的行数是 2026-11-14 的大概值，以代码为准。

## 1. 一句话

《影之刃零》（`PBZ.exe`，UE 5 shipping 版）的游戏内工具箱 MOD。

- 形式是 `version.dll` 代理：转发系统 `version.dll` 的全部导出。
- 在游戏引擎的 tick 里挂一个游戏线程回调，所有游戏操作都在那里做。
- 自己起一个工作线程，画 F10 面板（Win32 子窗口自绘，不碰游戏的 D3D）。

## 2. 构建产物

`CMakeLists.txt` 定义全部目标，Visual Studio 2022 x64，C++17，静态 CRT。

| 目标 | 产物 | 说明 |
|---|---|---|
| `version` | `build/Release/version.dll` | 完整 MOD |
| `version_triggers` | `build/triggers_only/version.dll` | 精简版，只关闭 DualSense 自适应扳机（`src/triggers_only/`） |
| `EnvCheck` | `build/Release/EnvCheck.exe` | 环境检查与修复（`src/env_check/`）。只从 System32 延迟加载，不会载入旁边的 `version.dll` |
| `pbz_conditions` · `pbz_weapon_actions` · `pbz_mod_panel` | 示例插件 DLL | `sdk/samples/`，也能用 `sdk/samples/CMakeLists.txt` 单独构建 |
| `*_test` · `combo_menu_preview` · `manual_ui_export` | 测试 exe | 见第 11 节 |

几组共用的源文件列表：

| 变量 | 内容 | 警告级别 |
|---|---|---|
| `PBZ_PLUGIN_HOST_SOURCES` | 插件宿主：`combo_plugins.cpp`、`mod_registry.cpp`、`plugin_ui.cpp` | 本项目的 `/W4 /WX` |
| `PBZ_LUA_SOURCES` | Lua 5.4.7 的 `.c`，按 C++ 编译，不含 `io` / `os` / `package` / `debug` / `linit` | `/W3 /WX-` |
| `PBZ_LUA_HOST_SOURCES` | `lua_host.cpp` + `PBZ_LUA_SOURCES` | 混合 |
| `PBZ_VENDOR_SOURCES` | UUEPBS 编辑器与 Dear ImGui | `/W3 /WX-`，C++20 |

`/W4 /WX` 只管本项目的代码，第三方代码保持原样，用自己的警告级别。

## 3. 目录地图

```text
src/
├─ core/          DLL 入口与转发、工作线程、命令文件、游戏线程钩子、UObject 反射、exe 识别、设置文件
├─ ui/            F10 面板宿主 debug_hub、主题 / 玻璃 / 动效、双语、快捷键、提示条、挡位胶囊、面板暂停、Donate 页
├─ character/     外观（NPC / BOSS 重定向）、招式派生、角色替换、身材 / 骨骼、骨骼变换
├─ plugin/        派生插件宿主、模块注册表、插件 UI 页模型、Lua 宿主
├─ boss/          Boss 连战、Boss AI、BOSS 援护、测试木桩、招式预览、入场 / 存档保护
├─ grab/          投技（魔渊擒拿等）
├─ air/           空中技能、空中闪避、JC 与奖励
├─ weapons/       武器预设、游戏内武器图标
├─ net/           联机测试：Steam 房间与消息、替身
├─ pad/           自适应扳机、面板打开时对游戏屏蔽手柄
├─ hair/          强制 Groom 头发
├─ supplement/    可选补充包的挂载与调用
├─ uuepbs/        UUEPBS 身材编辑器（第三方，见第 7 节）
├─ third_party/   Dear ImGui、Lua
├─ generated/     从游戏数据生成的表，不要手改（第 9 节）
├─ env_check/     EnvCheck.exe
└─ triggers_only/ 精简版 DLL
sdk/              插件接口头文件 include/pbz_combo_api.h、示例插件和示例 Lua 脚本
tests/            单元测试和离线绘制测试
installer/        Install.bat + install.ps1（终端安装器；打包时填入两端 PBZ.exe 的大小与 SHA-256）
samples/          sampleCombo.combo、UUEPBS 自带的三个示例预设
scripts/          公开工具：手册 PDF、EXE 适配、空 pak 挂载入口、特效表
licenses/         第三方许可全文
docs/             开发与玩家文档（第 13 节）
```

- `src/` 下每个子目录都是头文件搜索路径，源码里直接写 `#include "ue_runtime.h"`。
- `local/`、`build*/`、`dist/` 不进仓库。`.gitignore` 是白名单（先 `*`，再逐项 `!`），见第 10 节。

## 4. 运行时模型

### 4.1 加载

1. 游戏加载 `version.dll`。`DllMain`（`src/core/proxy.cpp`）只做转发准备，然后起工作线程 `Worker`。
2. 宿主不是 `PBZ.exe` 时什么都不做。
3. `game_build.cpp` 按 SHA-256 认 exe（Steam 版、WeGame 版各一个）。认不出时不读写任何游戏内存，只在标题画面提示版本不符；扳机钩子不依赖游戏布局，照常工作。
4. 没有 `PBZDebugMenu.ini` 时（第一次安装），`SeedSettingsOff` 先把几个默认开着的开关写成关，所以所有功能默认关闭。

### 4.2 两个线程

| 线程 | 谁在跑 | 能做什么 |
|---|---|---|
| 工作线程（面板线程） | `Worker` 主循环，约 16–25 ms 一圈：面板和各页的 `Tick()`；每秒一次读命令文件、找 `UGameEngine` 并安装游戏线程钩子 | 画界面、读快照、提交请求。**不调用任何引擎函数** |
| 游戏线程 | 引擎每帧 tick 开头的 `DispatchGameThread()`（`proxy.cpp`） | 所有 UObject 读写和引擎调用 |

游戏线程钩子（`game_thread.cpp`）把引擎对象的 vtable 指针换成私有副本，只替换 `Tick` 槽，不改可执行页、不改共享 vtable。

`DispatchGameThread()` 每帧按顺序调用各模块的 `OnGameThreadGuarded()`：

```text
MotianGrab → CharacterSwap → NpcRetarget → AirSkills → WeaponPresets → BossRush → BossAI → TestDummy
→ BossAssist → MovePreview → NetSteam → BoneMotionRuntime → NetPuppet → DebugSupplement → HubPause
→ TitleControl → GroomForce
```

- 每个模块自己包 SEH，出错几次后自动停用并写日志，不影响别的模块。
- Boss 连战转场期间（`BossRush::IsTransitioning()`），武器预设、测试木桩、援护、预览这几个模块暂停。
- 派生插件和 Lua 脚本的回调在 `NpcRetarget` 里由 `ComboPlugins::Frame` 驱动，同样只在游戏线程。

**线程边界是硬规则。** 头文件顶部注释写明每个函数属于哪个线程（Any thread / Game thread only / UI worker only），改之前先看。两边靠原子量、`Interlocked*`、小锁和「请求 / 快照」交换数据。

### 4.3 反射访问（`src/core/ue_runtime.h`）

- 调用 UFunction：`ue::Call(obj, "Fn").Set("Param", v).Invoke()`，返回值用 `Get<T>("ReturnValue")`。
- 属性按名字查偏移：`ue::Offset(class_or_struct, "Name")`（找不到返回 -1），再用 `ue::At<T>(obj, offset)` 读写。
- `FindObject(path)` 的结果有缓存。只有游戏线程会去问引擎自己的对象表；别的线程和没缓存的路径扫对象数组，慢。
- 固定地址和布局常量是按被接受的 exe 读出来的。游戏更新时按 [GAME_UPDATES.md](GAME_UPDATES.md) 重新定位。

### 4.4 命令文件和日志

- 游戏目录下的 `PBZDebugMenu.command`：工作线程每秒读一次，读完即删，按第一个词分发（`PollCommand`）。
  - 已有的词：`model`、`npcretarget`、`air`、`grab`、`weapons`、`boss`、`title`、`combo`、`bones`、`net`、`plugins`、`supplement`、`hair`、`triggers`、`status` 等。
  - 用途是脚本化测试：不用鼠标就能驱动各模块。
- 日志写在 `PBZDebugMenu.log`，上一次的在 `PBZDebugMenu.log.previous`。
  - 每条命令一行 `COMMAND <verb> ok=…`。
  - 各模块用大写标签开头：`NPC_COMBO`、`NPC_SKILL`、`NET_PUPPET`、`BOSS_STATUS`、`PLUGINS`……

### 4.5 设置文件

`PBZDebugMenu.ini` 由 `settings_store` 读写：

- 值缓存在内存里，`Put*` 只在值真正变化时写盘，调用方可以每次状态变化都存。
- 两个线程都可以用。
- 各节的含义见 README 的「设置文件」。

## 5. F10 面板

- 宿主 `src/ui/debug_hub.cpp`；页面在 `proxy.cpp` 里用 `DebugHub::Register(Page, 标题, 创建函数)` 注册。
- 当前的页：Boss 连战、Boss AI、角色替换、NPC 重定向、招式派生、空中技能 / JC、投技、毛发 / Groom、武器预设、快捷键、身材 / 骨骼、联机测试、Donate；装了补充包时还有调试补充。
- 页面是 Win32 子窗口，自己画（`ui_theme`、`ui_glass`、`ui_motion`、`ui_paint_buffer`）。不碰游戏的 D3D 和 swap chain。
- **双语**：界面文字一律写中文原文，由 `UiI18n` 按 `src/ui/ui_i18n_strings.h` 的 `{L"中文", L"English"}` 表翻译。新加的中文字符串必须进这张表，否则英文界面显示中文。
- **快捷键**：`ui_hotkeys`，键盘 / Xbox / PlayStation 三套，存在 ini 的 `[Hotkeys]`。

## 6. 招式派生

招式派生页是一张蓝图式画布：节点、连线、搜索、顶栏和每行的按键选择。它是改动最多的部分。

### 6.1 四层

| 层 | 文件 | 线程 | 职责 |
|---|---|---|---|
| 模型 | `npc_combo_model.h`（约 2300 行，纯头文件） | 任意，无引擎调用 | 节点类型、链结构、按键怎么走（`TriggerFor` / `Follow` / `StartButton` / `MoveRate`）、滞空查询、存档文本 `Encode` / `Decode` |
| 存取 | `npc_combo.cpp`、`combo_file.cpp` | 任意 | 每个外观一条链（主角自己也有一条），存在 `version.dll` 旁 `combos\*.combo`；当前文件由 ini `[NpcCombo] File` 决定；文件改动约一秒内重读，坏文件跳过 |
| 运行 | `npc_retarget.cpp`（约 1.2 万行） | 游戏线程 | 读按键 → 走链 → 施放 → 维持修饰节点 |
| 页面 | `combo_menu.cpp`（约 6000 行） | 工作线程 | 画布、搜索列表、顶栏、节点卡片 |

- 模型层由 `tests/npc_combo_test.cpp` 覆盖。改行为时先改模型、加测试，再改运行端。
- 页面能离线绘制：`combo_menu_preview`、`combo_library_test`、`manual_ui_export` 都把 `combo_menu.cpp` 编进测试 exe，用生成表代替游戏。

### 6.2 存档文本和兼容规则

- 链是一个 `Config`，节点在 `std::vector<Node> chain` 里。节点上限 `kMaxNodes = 10000`，按键分支等行数上限 `kMaxRoutes = 6`。
- 文本格式 `E1|T:…|S:…|I:…|…`：每个节点一段，按下标顺序写；最后是连到修饰节点的 `L:from,modifier`。完整说明在 `Encode` 前面的注释里。
- **兼容原则（必须保持）**：新节点类型写成旧版本认识的类型，再加一个小写字母标记。旧版本读到时节点下标不变，新节点变成不起作用的旧节点。已有的写法：

| 新类型 | 写成 |
|---|---|
| 滞空 | 不接线的间隔 + `f:` |
| 空中释放 | 不接线的间隔 + `j:` |
| 钩子 | 武器派生 + `g:` / `q:` / `r:` |
| 裁剪 | `W:` + `c:` |
| 空中派生 | 武器名 `_` 的武器派生修改 `O:` + `s:` |

- 加新标记前先 grep 已用过的字母。
- 测试里要有「去掉标记后旧版本怎么读」这一项。

### 6.3 一次按键怎么走（运行端）

1. **读按键**：`DriveHeroTriggers()` 把主角的按键读成 `NpcCombo::Press`。`bits` 是按钮位（Light 0x2、Heavy 0x4、Shachi 0x8、Special 0x10、CombatArt 0x20、Jump 0x40），`key` 是 NPC 技能键；`WithWeapon()` 补上当前主武器 id 和是否在空中。
2. **走链**：链在进行中时，由 `Follow` 决定下一步；没在进行中时，`TriggerFor` 找起端，顺序是空中派生（只在空中）→ 武器派生修改 → 普通起端。
3. **施放**：`PlayNode()` 施放节点。技能进 `PendingCast`，资源加载好后由 `TryPendingSkill()` 真正激活；外观、武器、碰撞、刀光都在这里准备。
4. **维持**：`DriveCombo()` 每帧推进计时、间隔、派生点，维持加速、霸体、无敌、伤害倍率等修饰效果。
5. **滞空**：`DriveAir()` 管链里施放的招式；`DriveOwnAir()` 管游戏自己放出、链没接管的主角技能。

### 6.4 外观、BOSS 技能与武器

- 生成表 `kProfiles`（外观重定向资料）、`kNpcSkillSets`（每个外观的技能），以及武器、挂点、碰撞、刀光表，各表的行和 `kNpcSkillSets` 一一对应。
- 当前外观是 `selected_`。**不变身**放别的 BOSS 的招式时，`BareRow()` 找到招式所属的行，记进 `weapon_skin_`。
- 和武器有关的取行一律用 `WeaponSkin()`（有 `weapon_skin_` 用它，否则用 `selected_`）。新加武器相关的东西时也要这样做。

### 6.5 加一个新节点

1. **模型**（`npc_combo_model.h`）：加 `NodeType` 值或已有类型上的标志，写工厂函数，在 `TriggerFor` / `Follow` 等处接入，`Encode` / `Decode` 用「旧类型 + 新标记」。
2. **测试**（`npc_combo_test.cpp`）：行为、开关、存档往返、旧版本读法。
3. **运行端**（`npc_retarget.cpp`）：在 `PressCombo` / `PlayNode` / `DriveCombo` 等处接入，日志打 `NPC_COMBO`。
4. **页面**（`combo_menu.cpp`）：`Button` 枚举（加在 `Count` 前）、工具顺序、`ItemKind`、搜索项、放置、标题和说明、行选择器、命中区域、存活判定。
5. **双语**：新的中文字符串进 `ui_i18n_strings.h`。
6. **插件接口**：要让插件能读写新节点的字段时，同步 `sdk/include/pbz_combo_api.h`、`combo_plugins.cpp` 和 [PLUGIN_API.md](PLUGIN_API.md)。
7. **文档**：`CHANGELOG.md`、`更新内容.txt`、[USER_MANUAL.md](USER_MANUAL.md)。

## 7. 派生插件和 Lua 脚本

接口给插件作者的说明在 [PLUGIN_API.md](PLUGIN_API.md)；这里只讲宿主内部怎么分层。

```text
sdk/include/pbz_combo_api.h      C ABI：插件看到的全部接口（PbzApi 函数表、结果码、能力、修订号）
        │
src/plugin/combo_plugins.cpp     宿主：载入 PBZPlugins\*.dll，实现 PbzApi，权限（[PluginGrants]），出错隔离，命令总线
        ├─ mod_registry.cpp      模块注册表：属性 / 动作 / 事件的反射表面，宿主本身不保存模块状态
        │     └─ module_apis.cpp → 各模块自己的 *_api.cpp（character/、boss/、air/、grab/、weapons/、pad/、hair/、ui/）
        ├─ plugin_ui.cpp         插件 UI 页的数据模型（标签、开关、滑条、选项、按钮、文字），由面板用自己的控件画
        └─ lua_host.cpp          Lua 5.4.7：PBZPlugins\scripts\*.lua 也是插件，id 为 lua.<文件名>，全局表 pbz / PBZ
```

- **默认关闭**：`[Plugins] Enabled`，或招式派生页顶栏的「派生插件」开关。关着时不载入任何东西。
- **线程**：插件回调只在游戏线程跑（`ComboPlugins::Frame`、`Emit`、`Decide`、`Run`）；面板只读宿主的视图；`PumpWorker()` 在工作线程处理属于面板的命令总线调用。
- **权限**：除了派生，每项能力都要玩家在列表里逐个插件授权；没授权的调用返回错误并写日志。
- **出错隔离**：插件调用出现访问违例时，这个插件在本次运行里停用。Lua 脚本每次回调有 20 万条指令预算（载入时 500 万），每个脚本 16 MB 内存上限；报错、超时、超内存都只停用这一个脚本。
- **模块描述表**：模块在自己目录的 `*_api.cpp` 里注册属性、动作和事件，宿主不改模块代码。给模块加可被插件读写的选项时改那个 `*_api.cpp`，再同步 PLUGIN_API.md 的「模块一览」。
- **接口修订**：改 C ABI 时提高修订号，旧插件必须照常载入（见 PLUGIN_API.md「兼容规则」）。

## 8. 身材 / 骨骼

| 文件 | 职责 |
|---|---|
| `src/character/npc_body_menu.cpp` | F10 面板「身材 / 骨骼」页：总开关、打开编辑器的按钮、状态 |
| `src/character/npc_bridge.cpp` | 把主角骨架（名字、父子表、姿势 buffer 偏移）交给 UUEPBS；姿势 finalize 时把这一帧交给编辑器写回滑条值；按主角速度选站 / 走 / 跑 / 冲刺分档 |
| `src/character/npc_body.cpp` | 读写引擎每帧算完的 component-space 姿势 buffer（UE5 `FTransform`，96 字节） |
| `src/character/npc_anim.cpp`、`npc_body_rig.cpp`、`npc_body_preset.cpp` | 骨骼动画时间轴、骨骼改写、预设读写 |
| `src/uuepbs/` | UUEPBS v1.3 编辑器和数据层（第三方，只改了 `win/slider_window.cpp`）。窗口用 CPU 渲染，不碰 D3D |
| `src/character/bone_motion*.{h,cpp}` | 骨骼变换节点的运行端 `BoneMotionRuntime`，和身材共用同一个姿势钩子 |

- 只改主角自己的身材；变成 BOSS 期间暂停。
- 姿势 finalize 路径上不许调引擎接口、不许分配内存，只碰组件自己的骨骼数组。
- 预设和动画在游戏目录的 `UUEPBS Presets\`。

## 9. 生成数据

`src/generated/*.generated.h` 都是脚本从游戏文件生成的：NPC 外观与技能、武器与挂点、碰撞与刀光、投技、Boss 目录、骨骼、UI 资源（图标、字体子集）、导出表等。

- **不要手改**。要改表，改生成脚本再重新生成。
- 大部分生成脚本依赖解包后的游戏数据，不在仓库里；公开的只有 `scripts/` 下的几个（例如 `gen_boss_effects.py`）。
- 这些表里的图标、字体子集和数据取自游戏文件，版权归原权利人所有，不在 MIT 许可范围内（见 [LICENSE](../LICENSE)）。
- `exports.generated.h` 和 `known_exes.h` 由 CMake 在构建时生成。

## 10. 编码约定

- **编译选项**：`/W4 /WX /utf-8`，静态 CRT。警告就是错误，包括 C4456（内层变量和外层同名）。
- **换行符**：仓库里 LF 和 CRLF 混用，部分文件带 UTF-8 BOM。改文件时保持原样，以实际字节为准（Python 数 `b'\r\n'`；Git Bash 的 `grep -c $'\r$'` 不可靠）。大文件如 `npc_retarget.cpp`、`combo_menu.cpp`、`npc_combo_model.h`、`ui_i18n_strings.h`、`README.md`、`CHANGELOG.md` 是 CRLF。
- **线程注释**：新函数在头文件里写明属于哪个线程。
- **默认关闭**：新功能没有 ini 时必须是关的。
- **出错不拖垮游戏**：游戏线程代码在模块的 SEH 保护里，失败写日志后停用自己。
- **文档用词**：`README.md`、`CHANGELOG.md`、`README.txt`、`更新内容.txt` 里不出现「菜单」两个字，用「面板」「页」「列表」。
- **两份更新记录**：
  - `CHANGELOG.md` 写给开发者：改了什么、怎么验证的、旧版本读到新存档会怎样。
  - `更新内容.txt` 写给玩家，用【新增】/【修复】，随安装包发布。
- **白名单 `.gitignore`**：新加的非 `.cpp` / `.h` 文件（文档、图片、脚本、Lua、许可文件）要加一行 `!路径`，否则会悄悄不进提交。提交前用 `git status --short --ignored` 查一遍。
- **不进仓库、不进安装包的东西**：游戏 EXE、资源包、补充包 `PBZDebugSupplement.*`、UE4SS、本地分析缓存、玩家的存档。

## 11. 测试

全部测试不启动游戏，输出在 `build/Release/`，看 `failures=0` 或退出码 0。

| 类别 | 测试 |
|---|---|
| 可以无人值守跑 | `npc_combo_test`、`combo_file_test`、`combo_save_stress_test`、`combo_library_test`、`combo_playback_rules_test`、`combo_hold_scope_test`、`combo_plugins_test`、`mod_registry_test`、`lua_host_test`、`game_build_test`、`game_thread_test`、`env_check_test`、`npc_*_test`、`grab_*_test`、`air_attack_reset_test`、`pad_*_test`、`game_pad_gate_test`、`supplement_layout_test`、`body_presets_test`、`bone_*_test`、`weapon_action_*_test`、`view_follow_state_test`、`trim_charge_runtime_test`、`append_log_test`、`net_puppet_rules_test`、`hair_controls_test` |
| 要参数 | `proxy_test <version.dll 的绝对路径> [--forward-only]`、`sample_combo_test <samples/sampleCombo.combo>` |
| **会操作窗口、焦点或输入，不要在无人值守时跑** | `input_focus_test`、`combo_menu_preview`、`manual_ui_export`、`combo_canvas_perf_test`、`ui_paint_buffer_test`、`supplement_sequence_test` |

- 存档相关测试用自己的临时目录，不碰玩家的 `combos\default.combo`。
- 完整列表和单独构建 SDK 示例的方法见 [BUILD.md](BUILD.md)。
- 离线通过不等于游戏里通过。实机效果（技能、过场、视觉）另行验证并记录。

## 12. 打包与发布

- `build.ps1 -Package` 写 `dist\CringeBladeZero_<日期>_<提交>.zip`，Steam / WeGame 通用：完整版和精简版 `version.dll`、`Install.bat` + `install.ps1`、`EnvCheck.exe`、`README.txt`、`更新内容.txt`、`LICENSE` 与第三方说明（`THIRD_PARTY_NOTICES.md`、`licenses\`）、示例插件和示例脚本（`PBZPlugins\`）、`SDK\`（接口头文件、示例源码、接口手册、`AGENT_GUIDE.md`）、UUEPBS 示例预设、`sampleCombo.combo`、部分 `docs\` 和手册图片。文件清单就是 `build.ps1` 里的 `$files` 表，加文件改那里。
- 安装器在打包时写入两端 `PBZ.exe` 的大小和 SHA-256（来自 `proxy.cpp` 的常量）。游戏更新后要先改那些常量。
- GitHub 发行版的 README 链接指向 `releases/latest/download/CringeBladeZero.zip` 和 `CringeBladeZero_Manual_ZH.pdf`，所以每个发行版都要带这两个固定名字的附件。
- 手册 PDF 由 `scripts/build_manual.py` 生成（需要 reportlab），每种节点都要有 `docs/manual_images/node_<类型>.png`。

## 13. 文档索引

| 文档 | 给谁 | 内容 |
|---|---|---|
| [AGENTS.md](../AGENTS.md) | AI 助手、新开发者 | 硬规则、命令、从哪开始读 |
| 本文 | 开发者 | 代码结构和运行时模型 |
| [BUILD.md](BUILD.md) | 开发者 | 构建依赖、测试、打包、安装和回滚 |
| [GAME_UPDATES.md](GAME_UPDATES.md) | 开发者 | 游戏更新后重新定位地址、字节核对、两端差异 |
| [PAK_WORKFLOW.md](PAK_WORKFLOW.md) | 开发者 | 独立 pak 制作、挂载、补充容器 |
| [PLUGIN_API.md](PLUGIN_API.md) | 插件作者 | C 接口与 Lua 接口的完整手册 |
| [AGENT_PLUGIN_GUIDE.md](AGENT_PLUGIN_GUIDE.md) | 插件作者、AI 助手 | 插件和脚本的模板、安装、授权、排错速查 |
| [USER_MANUAL.md](USER_MANUAL.md) | 玩家 | 使用手册（PDF 版的来源） |
| [WEAPON_DERIVATIONS.md](WEAPON_DERIVATIONS.md)、[ANIMATION_PRESETS.md](ANIMATION_PRESETS.md)、[SOFT_BONES.md](SOFT_BONES.md) | 玩家 | 单项功能说明 |
| [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) | 所有人 | 第三方代码与许可 |
