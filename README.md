<div align="center">

<img src="docs/title.jpg" alt="豪之刃 · Cringe Blade Zero" width="100%">

# 豪之刃 · Cringe Blade Zero

**《影之刃零》（Phantom Blade Zero）单机游戏内工具箱**

一个 `version.dll` 代理 MOD：游戏内自绘面板、Boss 连战、Boss 投技、空中技能、武器预设与挡位系统，以及用 75 个 NPC / 人形 Boss 外观操作主角。

![Platform](https://img.shields.io/badge/platform-Windows%20x64-1f2328?style=flat-square)
![Language](https://img.shields.io/badge/C%2B%2B-17-00599C?style=flat-square)
![Engine](https://img.shields.io/badge/Unreal%20Engine-5-0e1128?style=flat-square)
![Build](https://img.shields.io/badge/build-CMake%20%2B%20MSVC-d8c292?style=flat-square)

**[⬇ 下载最新版（CringeBladeZero.zip）](https://github.com/Jersyi/cringe-blade-zero/releases/latest/download/CringeBladeZero.zip)** · [全部版本](https://github.com/Jersyi/cringe-blade-zero/releases) · [❤ Donate](#donate--支持作者)

MOD 完全免费，不打赏也可以使用全部功能。喜欢的话可以在[爱发电](https://afdian.com/a/Jersyi1)支持作者（自愿）。

</div>

---

## 功能

[完整功能与全部节点手册（Markdown）](docs/USER_MANUAL.md) · [中文 PDF 下载](https://github.com/Jersyi/cringe-blade-zero/releases/latest/download/CringeBladeZero_Manual_ZH.pdf)

PDF 共 58 页，含 35 张节点与变体实图、9 张 UI 图、31 类节点逐项说明、sampleCombo及连线/计时规则。界面图由真实自绘控件离线渲染。PDF 也包含在最新版安装包内。

| 模块 | 内容 |
|---|---|
| **面板（Hub）** | 无边框自绘界面，仿游戏 UI 的墨色与金线风格；中英双语；打开时可暂停游戏 |
| **快捷键** | 键盘、Xbox、PlayStation 各一套，自动识别当前手柄和最近使用的设备，提示显示对应按键图标 |
| **Boss 连战** | 传送到任意 Boss、直接触战、连战流程，附难度与周目设置；入场保护与存档保护 |
| **Boss AI** | 读取并调整当前 Boss 实例的原生 AI 参数 |
| **投技** | 53 招人形 Boss 原生投技，按投技快捷键触发（默认键盘 G、Xbox Y + B、PS 三角 + 圈）；施放时换成该 Boss 的武器，并由不可见的 Boss 骨架代理重放动画，武器演出与 Boss 一致；可放大伤害、无视无敌；受击、倒地时也能触发；可选「投技时变身为该 Boss」：先换成这招 Boss 的外观再投，投完换回 |
| **空中技能 / JC** | 空中施放战技与杀气技、空中闪避、跳跃打断、近敌 JC 弹跳后重置空中普攻限制、资源奖励、缓落 |
| **武器预设** | 两套装备一键切换（默认键盘 Z、Xbox LB + L3、PS 触控板左右滑），显示游戏内武器图标与官方名称，重启后保留 |
| **挡位** | 右下角 0–10 档胶囊：挡位越高开启越多（JC、投技、奖励、伤害倍数、无视无敌），10 档滑条燃起游戏手绘火焰 |
| **角色 / NPC** | 主角兼容装束替换；以 NPC / 人形 Boss 外观操作主角（75 个）：保留主角的动作与能力，手臂、武器、碰撞半径和相机按 NPC 体型适配；过场动画里外观套在游戏的过场主角上，过场结束后回到主角 |
| **NPC 技能** | 套用 NPC 外观后按 NPC 技能快捷键（默认键盘 X、手柄 L3 + R3）施放该 Boss 的技能（随机轮换、自动锁定、右下角提示），技能期间换成 Boss 自己的武器；伤害倍率可调（默认 3 倍，NPC 页选择）；Boss 蓝图里专有的特效和动作在主角身上重放（龙柱、魔渊柱、武器追踪、反击闪光）；技能期间 Boss 武器和身体的攻击判定复制到主角身上，武器拖尾 / 粒子特效显示在 Boss 武器上；长鸿·雷电枪骑的马播放 Boss 自己的动作；Boss 的慢放招式（如梦华圣母、虚空的时停）由主角施放时，主角保持正常速度，改为慢放周围 100 米内的其他角色（投技里被投的目标和主角同步，不被放慢；BOSS 援护和招式预览里 Boss 放的慢放同样不会让主角变慢） |
| **招式派生** | 31 种节点类型（含 BOSS 特效）：BOSS 技能、武器派生、投技、援护、分支、组合、加速曲线、裁剪与按住停留、空中派生、骨骼变换等。右侧节点/武器两页，搜索、预览、复制粘贴、多表保存与 sampleCombo 示例。每次到达节点才开始间隔/停顿计时，完整链尾结束后才接受隐式重启。 |
| **毛发 / Groom** | 头发表示切换、强制 Groom、GPU 包围盒调试 |
| **调试补充（可选）** | 装有补充包时出现：游戏原版的 169 项调试功能重新整理为 9 个分区的卡片（无敌、不死、飞行、移速、传送、DLSS、音频、物品、天赋、剧情推进、过场、性能采集等），可搜索、按分区筛选；会改写存档的项目须在三秒内再点一次确认 |
| **Donate** | 面板最底部的分页：作者的爱发电卡片（二维码）和链接，可直接在浏览器中打开或复制链接。打赏完全自愿，不影响任何功能 |
| **其他** | DualSense 自适应扳机可关闭；全部设置写入 `PBZDebugMenu.ini` |

## 本地替身实验（开发中）

进入场景后打开「联机测试」页，不加入房间，点击「生成替身」再正常出招。替身使用联机相同的姿势编码，复制角色部件、显示状态，以及当前支持的 Niagara / Cascade 特效。火流星链条的曲线绑定和曲线点也参与同步。新版还复制角色拥有的活动子弹外观与轨迹；这不代表子弹伤害已联网。

点击「本地对战判定 / 重置」，替身会面对主角。双方各有 1000 点独立实验血量，武器轨迹发生接触时每次攻击最多扣 100 点；归零后停止结算。点击「结束对战判定」恢复普通镜像。此实验用于检查攻击轨迹与双向命中，不改变游戏生命值；原生受击、格挡、闪避无敌、投射物和网络对战裁定尚未接入。

特效仍有覆盖限制：没有角色归属的世界特效、短于采样间隔的效果，以及尚未支持的对象参数和动态材质参数，可能不同步。当前实测以 Steam 单机本地替身为准，未完成双机对战验证；双方联机时需要安装同一版 MOD。测试不改连招文件，也不需要额外 pak。

## 骨骼变换

独立「骨骼变换」页按体型、骨骼详情和骨架调试分区，位移、旋转、缩放和惯性参数使用自绘滑条，可配合精确数值输入。招式派生中也可添加同名节点，连接规则与加速节点相同，每个节点最多调整 32 根骨骼，悬停预览同步应用。

女性预设包含胯部转动、低强度臀胯整体跟随、胸腔反向转动，以及可用胸部 / 臀部骨骼的惯性运动。可选 [PBZSoftBones 补充包](docs/SOFT_BONES.md) 为现有 75 个外观条目及主角提供带真实权重的胸臀骨骼，含沐小葵两个阶段；开关默认关闭，关闭后恢复原身体。不安装补充包时，缺失项仍会跳过。参数和参考文章见[骨骼变换与动画预设指南](docs/ANIMATION_PRESETS.md)。当前采用显式参数解算，未训练 ONNX。

## 安装

完整版安装器同时安装作者示例 `combos/sampleCombo.combo`，也可在安装器里选 5 单独安装。到招式派生页的文件列表选择示例；已有同名编辑版会保留，新文件另存名称。原 default 和当前文件选择不会被覆盖。详见 [示例说明](samples/README.md)。

本版移除“魂（女性化）”外观及其专用 pak 挂载入口，发布包不含女性化主角资源。通用骨骼编辑、女性动作预设和可选胸臀补充功能继续保留。

Steam / WeGame 共用同一个 `version.dll` 和通用发布包，无需分别下载。

1. 先退出游戏。下载[发布包](https://github.com/Jersyi/cringe-blade-zero/releases/latest/download/CringeBladeZero.zip)并解压，双击 `Install.bat`：
   - 安装器从注册表找到游戏目录（Steam 卸载项、Steam 库列表或以游戏命名的卸载项；找不到时可手动输入游戏目录或 `PBZ.exe` 所在文件夹），并校验游戏版本（Steam 版或 WeGame 版）。找到多个安装时先让你选一个。
   - 按 **1** 安装完整版，**2** 安装精简版（只关闭 DualSense 自适应扳机），**3** 卸载，**4** 环境检查与修复（见下文）。
   - 复制后校验 SHA-256。目录里已有其他 MOD 的 `version.dll` 时先备份，卸载时还原。
   - 界面语言随 Windows 显示语言：中文系统显示中文，其他系统显示英文。

   也可以手动安装：把 `version.dll` 复制到 `PBZ.exe` 所在的文件夹。Steam 版是

   ```text
   <PBZ 安装目录>\platform\steam\version.dll
   ```

   WeGame 版放在 WeGame 游戏目录里 `PBZ.exe` 的旁边。

2. 启动游戏。进入游戏时屏幕上方出现“豪之刃”横幅，按 **F10** 打开面板（手柄：Xbox 同时按 **View + Menu**，PlayStation 同时按 **Create + Options**）。

3. 可选：把补充包 `PBZDebugSupplement.pak`、`.utoc`、`.ucas` 放进 `<PBZ 安装目录>\ProjectPBZ\Content\Paks\~mods\`，面板会多出“调试补充”页。补充包由更新前的游戏文件生成，不随发布包提供；没有它时 DLL 照常工作。

### 环境检查与修复

游戏起不来、面板没出现或设置存不下时，在 `Install.bat` 里选 **4**，或直接双击发布包里的 `EnvCheck.exe`（也可以把 `PBZ.exe` 拖到它上面，或运行 `EnvCheck.exe --game "<PBZ.exe 所在文件夹>"`）。它逐项检查并给出结论：

| 范围 | 检查内容 |
|---|---|
| 系统 | Windows 版本；VC++ 2015–2022 x64 运行库（游戏需要，低于 14.40 时提示更新；MOD 本身静态链接）；Direct3D 12 显卡；MOD 导入的全部系统 DLL 与函数，以及转发目标 `version.dll` 的导出；XInput 与已连接的手柄；面板字体 |
| 安全与加载 | 会让游戏不加载 MOD 的 Windows 设置：KnownDLLs 里的 `version.dll`、Exploit protection 里 `PBZ.exe` 的“首选系统映像”“代码完整性保护”等、映像劫持（IFEO Debugger）、智能应用控制、攻击面减少规则；已装的安全软件；最近 30 天代码完整性与 Microsoft Defender 拦截 `version.dll` 的记录 |
| 每个游戏安装 | `PBZ.exe` 版本（Steam / WeGame，校验 SHA-256）；已装的 `version.dll`（完整版、精简版、其他 MOD，是否与本包一致）与它“来自网络”的下载标记；放错位置的副本（游戏根目录、`Binaries` 等不会被加载的位置）；其他 MOD 加载器与游戏目录里的旧运行库；写入权限与只读文件；`PBZDebugMenu.ini`；`combos\` 里每个 `.combo` 文件的校验；日志里上次运行的状态 |
| MOD 是否加载 | 游戏正在运行时，读取它实际加载的 `version.dll`（正在运行的 `PBZ.exe` 不在注册表里也会被检查）；没在运行时，用游戏自己的日志时间判断上次运行有没有加载 MOD |

发现问题时按编号修复（**A** 全部修复，**R** 重新检查）：

- 安装或更新完整版 MOD（其他 MOD 的 `version.dll` 先备份）
- 去掉 `version.dll` 上“来自网络”的标记
- 把放错位置的副本改名停用
- 从微软官方下载最新 VC++ 运行库，校验微软签名后安装
- 让 Steam 验证游戏文件
- 取消只读、把损坏的 `.combo` 文件移到 `combos\broken`
- 给当前用户授予 `PBZ.exe` 所在文件夹的修改权限
- 运行系统文件修复 `sfc /scannow`
- 打开 Windows 更新或语言设置（驱动、字体），或 Windows 安全中心的相关页面（智能应用控制、Exploit protection、病毒和威胁防护）

需要管理员权限的修复会弹出 UAC 确认，下载、改权限和系统修复执行前还会再确认一次。检查本身只读，不改动任何文件；结果同时保存为 `EnvCheck_report.txt`，可以发给作者排查。`--dry-run` 只列出修复会做什么。

**所有检查都正常，但 MOD 还是没有加载**：启动游戏、停在标题画面，再运行一次 `EnvCheck.exe`，它会直接读出游戏加载的是哪个 `version.dll`。常见原因是启动的不是被检查的那份 `PBZ.exe`（另一个游戏平台或另一份安装，把它拖到 `EnvCheck.exe` 上检查），或安全软件（360、腾讯电脑管家等）、智能应用控制、Exploit protection 拦下了 `version.dll`。安全设置只给出位置和打开入口，不会自动修改。

> MOD 只在匹配的 `PBZ.exe` 版本上启用（Steam 版和 WeGame 版，启动时校验 SHA-256）。版本不符时（通常是游戏更新后）不读写任何游戏数据，只保留 DualSense 自适应扳机开关，并在画面上方提示“豪之刃 未适配当前游戏”。当前版本对应 2026-11-04 更新后的游戏。日志写在 DLL 旁的 `PBZDebugMenu.log`。

## Donate · 支持作者

<img src="docs/donate.jpg" alt="爱发电 · Jersyi" width="280" align="right">

**豪之刃完全免费，不打赏也可以使用全部功能**，没有任何功能需要付费或打赏才能解锁。

如果你喜欢它、想支持作者继续更新，可以在爱发电为我发电（完全自愿，非常感谢！）：

- 链接：**<https://afdian.com/a/Jersyi1>**
- 或用手机扫描右侧二维码。
- 游戏里按 **F10** 打开面板，最底部的 **Donate** 页也有同样的二维码和链接。

<br clear="right">

## 构建

需要 **Visual Studio 2022**（含 C++ 桌面开发与 MASM）和 **CMake ≥ 3.24**。

```powershell
.\build.ps1            # 构建全部目标（Release）
.\build.ps1 -Package   # 另外打包 dist\CringeBladeZero_<日期>_<提交>.zip
```

或手动：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target version
```

发布包内含完整版与精简版 `version.dll`、一键安装 `Install.bat` 与 `install.ps1`、环境检查与修复 `EnvCheck.exe`、安装说明 `README.txt`（含默认快捷键）和玩家版更新内容 `更新内容.txt`。

| 目标 | 产物 |
|---|---|
| `version` | `build/Release/version.dll`，完整 MOD |
| `version_triggers` | `build/triggers_only/version.dll`，只关闭自适应扳机的精简版 |
| `EnvCheck` | `build/Release/EnvCheck.exe`，环境检查与修复。接受的 `PBZ.exe` 版本在构建时从 `proxy.cpp` 生成（`known_exes.h`）；启动时只加载 kernel32 / advapi32 / bcrypt，其余延迟加载且只从 System32 加载，旁边的 `version.dll` 不会被载入 |
| `env_check_test` | 环境检查的纯逻辑测试：PE 导入 / 导出表、DLL 识别、日志里上次运行的状态、SHA-256、生成的版本表 |
| `proxy_test` · `hair_controls_test` · `game_thread_test` · `pad_triggers_test` · `pad_gate_test` · `game_pad_gate_test` · `input_focus_test` · `npc_pose_transfer_test` · `npc_combo_test` · `combo_file_test` · `combo_menu_preview` | 测试（`input_focus_test` 用假 UnrealWindow 线程验证面板焦点、鼠标捕获与按键投递，三套快捷键的录制、触发、旧 INI 迁移和设备识别，以及用手柄打开面板、切换页面、移动焦点、确认和返回；`pad_gate_test` 验证面板打开时游戏读到的手柄状态（按住的键、打开面板的组合键、XInput 与 DualSense 报文）；`game_pad_gate_test` 在替身程序上验证这些导入表钩子；`npc_pose_transfer_test` 用合成骨架验证 NPC 姿势重定向、双手握持、武器缩放与坐骑代理；`npc_combo_test` 验证招式分类、主角动作标签、连线顺序、按键分支的走向（轻轻重、轻轻轻、停 2 秒再按）、派生点与停顿计时、加速 / 霸体 / 无敌 / 允许中断 / 武器属性 / 保持派生的作用范围（无敌和保持派生的存档）、间隔与慢放倍率、投技、BOSS 技能、BOSS 援护（按键穿过援护继续放后面的招、援护在末尾时自己算一招、修饰节点和存档）、武器派生和伤害倍率节点、组合（顺序、嵌套、修饰节点的作用范围、存档）和瞬移节点（距离与高度、落点、正面站不下时绕到侧面和缩短距离）、钩子节点（两种方式、存档、旧版读成武器派生，最远距离和钩子速度的存档、范围和与加速相乘）、滞空和空中释放（取值、只有空中放出的招式用空中释放、开关、钩子和瞬移不受影响、存档和旧版读成间隔、不能删除）、空中派生（空中优先于普通起端、NPC 技能键行、开关、存档和旧版读成没选武器的武器派生修改）、主角自己的技能的滞空和空中释放、击飞高度、按住时间（分支的点按行和按住行、只有按住的行、按住的起端、存档与范围限制）、自定义组合键（按顺序的间隔判定、一起按、存档、只接自己的派生链、继续派生键）、自定义按键（单键、按顺序、一起按、存档、吞掉设置和继续派生键）、加速的速率保持、加速曲线（插值、平均速度、存档）、不变身存档、主角处决的武器筛选和派生链存档（包括旧格式）；`combo_library_test` 在隐藏窗口中检查双分页、分类和翻页布局（不注入输入）；`combo_save_stress_test` 在新建的独立目录中压力测试节点保存、重读、另存为、并发读取及损坏文件保护，不使用玩家的 `default.combo`；`combo_file_test` 验证 `.combo` 文件的压缩与校验、损坏文件跳过、INI 迁移、另存为（含另存为窗口给出的 `combos\` 内外路径）与切换文件和改动后重新载入；`combo_menu_preview` 用生成表离线绘制招式派生页，并用脚本化的鼠标和键盘操作检查拖动节点、连线、断线、搜索、替换按键与按键分支的选键、停顿和加行、加速 / 霸体 / 无敌 / 允许中断 / 武器属性的箭头规则与倍数输入、箭头在各缩放下和被节点遮挡时是否画出、招式派生点的调节与输入、投技节点的选择（全部 Boss 的投技，当前 Boss 在前）、BOSS 技能节点的选择（先选外观再选招式）、BOSS 援护节点的选择与删除、招式预览卡片（停留 0.4 秒出现、移开收起；搜索框里方向键选中或鼠标指着的招式行，卡片在搜索框旁边不遮住列表）、武器列表的点选和拖放、武器派生节点的武器与招式选择、伤害倍率的步进和输入、主角处决的武器栏、组合框的拖入拖出、连线限制、移动与调整大小、瞬移节点的距离和高度选择、自定义组合键的选键、删键和按法 / 间隔输入、自定义按键的录制（键盘和手柄）、手柄按键选择、吞掉开关、继续派生键的选择和录制、派生提示开关、加速曲线（勾选、曲线窗口里拖点、双击加点、右键删点、关闭后保留曲线）、投技与 BOSS 技能的不变身开关、投技预览、测试木桩按钮、无视防御按钮、保持派生按钮与时间步进、按摇杆方向闪避按钮、派生表切换、文件选择与另存为（在 Windows 自带的另存为窗口里输入文件名）、拖入招式、平移、缩放、删除，以及 Ctrl / Shift 框选、Ctrl + 点加选、选中节点一起拖动和删除） |

编译选项：`/W4 /WX /utf-8`，静态 CRT。

## 默认快捷键

| 操作 | 键盘 | Xbox | PlayStation |
|---|---|---|---|
| 释放投技 | `G` | `Y` + `B` | `△` + `○` |
| 施放 NPC 技能 | `X` | `L3` + `R3` | `L3` + `R3` |
| 切换武器预设 | `Z` | `LB` + `L3` | 触控板左右滑动 |
| 提升 / 降低挡位 | `=` / `-` | — | — |
| 显示 / 隐藏面板 | `F10` | `View` + `Menu` | `Create` + `Options` |
| 返回 / 隐藏面板 · 切换页面 | `Esc` · `Ctrl` + `Tab` | `B` · `LB` / `RB` | `○` · `L1` / `R1` |
| 毛发页 · 蒙皮 / Groom 切换 | `F7` · `F8` | — | — |
| 强制 Groom · 套装解锁 | `F6` · `Shift` + `F6` | — | — |
| GPU 包围盒 · 自适应扳机 | `F9` · `Shift` + `F9` | — | — |

键盘默认键避开了游戏自己的键位（WASD、E、F、Q、R、T、V、空格、Shift、Ctrl、1、2 等）。手柄的每个键游戏都在用，所以手柄快捷键都是两键同时按（220 毫秒内）。

每个功能有键盘、Xbox、PlayStation 三套绑定，都可在面板“快捷键”页的三列里重新录制：

- 键盘那套始终有效。
- 手柄只用当前手柄那一套。“手柄类型”默认“自动”，按最近按下的手柄识别（经 Steam Input / DS4Windows 映射的 DualSense 也识别为 PlayStation），也可固定为 Xbox 或 PlayStation。
- 屏幕上的提示跟随最近使用的设备：按过键盘或鼠标就显示键盘键，按过手柄就显示该手柄的按键。

面板可以全程用手柄操作：

- 十字键或左摇杆上下在控件之间移动（列表里先移动选中项），左右调整当前控件（下拉框换选项、切换语言），在左侧分页列表里按右进入页面。
- A（叉）确认：按下按钮、勾选、展开下拉框；在快捷键框上按 A 开始录制。
- B（圈）返回：收起下拉框 → 回到分页列表 → 隐藏面板。
- LB / RB（L1 / R1）切换页面，LT / RT（L2 / R2）滚动页面。
- 面板打开时游戏收不到手柄按键，手柄快捷键也不触发；关闭面板时还按着的键要松开后才会再传给游戏。

快捷键也可以直接在 `PBZDebugMenu.ini` 的 `[Hotkeys]` 里改，重启游戏生效：

- 每个功能三项：`GrabRelease=kb:G`（键盘）、`GrabRelease.Xbox=xb:Y+B`、`GrabRelease.PS=ps:Triangle+Circle`。首次启动写入全部默认值。
- 值可以不带 `kb:` / `xb:` / `ps:` 前缀，空格随意，例如 `ToggleHub=Ctrl + F10`、`NpcSkill.Xbox=LB + RB`；留空表示不绑定。
- 打开面板的 `ToggleHub` 留空或写错时仍用 F10。
- 旧版每个功能只有一项；升级后自动迁移：原手柄组合键同时用于 Xbox 和 PlayStation 两套，键盘用新默认键。

## 设置文件

`PBZDebugMenu.ini` 与 DLL 放在一起，修改时才写盘（`[Hotkeys]` 例外：首次启动写入全部快捷键的默认值）。第一次启动时还没有这个文件：MOD 新建它，并把原本默认开着的开关（关闭自适应扳机、打开面板时暂停游戏、派生提示、黑伤处决改为魔渊擒拿）写成关，所以所有功能默认关闭。

| 节 | 内容 |
|---|---|
| `[UI]` | 界面语言 |
| `[Hub]` | 打开面板时暂停游戏 |
| `[Hotkeys]` | 快捷键：每个功能 `<功能>`（键盘）、`<功能>.Xbox`、`<功能>.PS` 三项；`PadGlyphs` 为手柄类型（`auto` / `xbox` / `ps`）；`ToggleHub` 为打开面板的键（默认 `kb:F10`、`xb:View+Menu`、`ps:Create+Options`） |
| `[Gear]` | 当前挡位 |
| `[Grab]` · `[GrabExtras]` · `[GrabDamage]` | 投技招式与开关、换武器 / 伤害模式 / 无视无敌、各招实测伤害 |
| `[NpcSkill]` | NPC 技能伤害倍率 `DamageMultiplier`（1–10，默认 3） |
| `[NpcCombo]` | `File`：当前使用的派生文件（`combos\<File>.combo`，默认 `default`）。派生链本身存在 DLL 同目录的 `combos\` 文件夹；以前存在这一节的派生链在第一次运行时搬进 `combos\default.combo` |
| `[AirSkills]` | 空中施放、近敌 JC、缓落与奖励 |
| `[WeaponPresets]` | 两套武器预设与当前套 |

## 项目结构

```text
src/
├─ core/          DLL 入口与转发（proxy.cpp、forwarders.asm、version.def）、游戏线程钩子、UObject 反射、设置文件
├─ ui/            面板、主题、动画、输入焦点与鼠标捕获、双语、快捷键、横幅、挡位胶囊、面板暂停、技能提示、Donate 页
├─ boss/          Boss 连战、Boss AI、入场保护与存档保护
├─ grab/          投技：原生成对激活、状态租约、Boss 武器与姿态代理、伤害
├─ air/           空中技能、空中闪避、JC 与奖励
├─ weapons/       武器预设与游戏内武器图标
├─ character/     角色替换、NPC 外观重定向（姿势传递、武器适配、碰撞体、技能、坐骑代理）
├─ hair/          毛发显示与 Groom
├─ supplement/    可选补充包：运行时挂载、调用原版调试蓝图、调试补充页
├─ pad/           DualSense 自适应扳机；面板打开时对游戏屏蔽手柄
├─ generated/     从游戏数据生成的表（投技目录、武器、骨骼、NPC 档案 / 技能 / 坐骑、UI 资源、导出表）
├─ env_check/     环境检查与修复 EnvCheck.exe
└─ triggers_only/ 精简版 version.dll
tests/            proxy / 毛发 / 游戏线程 / 扳机 / 输入焦点 / NPC 姿势 / 补充页目录测试
installer/        一键安装（Install.bat 启动 install.ps1；打包时填入 Steam 版与 WeGame 版 PBZ.exe 的大小与 SHA-256）
docs/             编译、更新适配、独立 pak 指南，以及 README 图片
scripts/          公开的 EXE 适配工具、空 pak 挂载入口制作工具
```

每个 `src/` 子目录都是头文件搜索路径，源码里直接写 `#include "ue_runtime.h"`。

## 开发文档

- [编译、测试与打包](docs/BUILD.md)：构建依赖、测试、Steam / WeGame 通用包和安装回滚。
- [游戏更新适配](docs/GAME_UPDATES.md)：地址定位、字节核对、两端差异、资源表和验证流程。
- [独立 pak 制作与使用](docs/PAK_WORKFLOW.md)：三件套挂载、自制资源 Cook、补充容器重组和 DLL 接入。

公开辅助工具在 `scripts/`；[资源解包密钥](docs/PAK_WORKFLOW.md#解包密钥)已记录在独立 pak 指南中。游戏 EXE、资源包及本地分析缓存不随文档上传。

## 更新日志

玩家看的更新内容见 [更新内容.txt](更新内容.txt)（随安装包发布）；开发记录见 [CHANGELOG.md](CHANGELOG.md)（不进安装包）。

## 说明

- 仅用于单机游戏，与游戏开发商无关。
- `src/generated/` 内的图标、字体子集与数据表取自游戏文件，版权归原权利人所有。


替身头发默认 Groom，可在联机页「替身头发」改为发片；缺少匹配 Groom 时自动回退，不影响主角的头发设置。骨骼页提供自绘搜索列表（排除表情骨骼）和实时全身骨架调试。具体说明见 [骨骼与动画预设](docs/ANIMATION_PRESETS.md)。

骨骼编辑新增简化体型滑杆、子骨骼模式和参考姿势镜像，保留惯性抖动与女性预设。设计参考 XTGMods 的 UUEPBS，署名及授权核查范围见 [第三方说明](THIRD_PARTY_NOTICES.md)。武器派生修改支持长按与格挡组合输入，同名武器版本分开列出，使用方法见 [武器派生与特殊输入](docs/WEAPON_DERIVATIONS.md)。
