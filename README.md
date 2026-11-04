<div align="center">

<img src="docs/title.jpg" alt="豪之刃 · Cringe Blade Zero" width="100%">

# 豪之刃 · Cringe Blade Zero

**《影之刃零》（Phantom Blade Zero）单机游戏内工具箱**

一个 `version.dll` 代理 MOD：游戏内自绘面板、Boss 连战、Boss 投技、空中技能、武器预设与挡位系统，以及用 75 个 NPC / 人形 Boss 外观操作主角。

![Platform](https://img.shields.io/badge/platform-Windows%20x64-1f2328?style=flat-square)
![Language](https://img.shields.io/badge/C%2B%2B-17-00599C?style=flat-square)
![Engine](https://img.shields.io/badge/Unreal%20Engine-5-0e1128?style=flat-square)
![Build](https://img.shields.io/badge/build-CMake%20%2B%20MSVC-d8c292?style=flat-square)

**[⬇ 下载最新版（CringeBladeZero.zip）](https://github.com/Jersyi/cringe-blade-zero/releases/latest/download/CringeBladeZero.zip)** · [全部版本](https://github.com/Jersyi/cringe-blade-zero/releases)

</div>

---

## 功能

| 模块 | 内容 |
|---|---|
| **面板（Hub）** | 无边框自绘界面，仿游戏 UI 的墨色与金线风格；中英双语；打开时可暂停游戏 |
| **快捷键** | 键盘、Xbox、PlayStation 各一套，自动识别当前手柄和最近使用的设备，提示显示对应按键图标 |
| **Boss 连战** | 传送到任意 Boss、直接触战、连战流程，附难度与周目设置；入场保护与存档保护 |
| **Boss AI** | 读取并调整当前 Boss 实例的原生 AI 参数 |
| **投技** | 53 招人形 Boss 原生投技，按投技快捷键触发（默认键盘 G、Xbox Y + B、PS 三角 + 圈）；施放时换成该 Boss 的武器，并由不可见的 Boss 骨架代理重放动画，武器演出与 Boss 一致；可放大伤害、无视无敌；受击、倒地时也能触发 |
| **空中技能 / JC** | 空中施放战技与杀气技、空中闪避、跳跃打断、近敌 JC 弹跳与资源奖励、缓落 |
| **武器预设** | 两套装备一键切换（默认键盘 Z、Xbox LB + L3、PS 触控板左右滑），显示游戏内武器图标与官方名称，重启后保留 |
| **挡位** | 右下角 0–10 档胶囊：挡位越高开启越多（JC、投技、奖励、伤害倍数、无视无敌），10 档滑条燃起游戏手绘火焰 |
| **角色 / NPC** | 主角兼容装束替换；以 NPC / 人形 Boss 外观操作主角（75 个，实验）：保留主角的动作与能力，手臂、武器、碰撞半径和相机按 NPC 体型适配 |
| **NPC 技能** | 套用 NPC 外观后按 NPC 技能快捷键（默认键盘 X、手柄 L3 + R3）施放该 Boss 的技能（随机轮换、自动锁定、右下角提示），技能期间换成 Boss 自己的武器；伤害倍率可调（默认 3 倍，NPC 页选择）；Boss 蓝图里专有的特效和动作在主角身上重放（龙柱、魔渊柱、武器追踪、反击闪光）；技能期间 Boss 武器和身体的攻击判定复制到主角身上，武器拖尾 / 粒子特效显示在 Boss 武器上；长鸿·雷电枪骑的马播放 Boss 自己的动作 |
| **毛发 / Groom** | 头发表示切换、强制 Groom、GPU 包围盒调试 |
| **调试补充（可选）** | 装有补充包时出现：游戏原版的 169 项调试功能重新整理为 9 个分区的卡片（无敌、不死、飞行、移速、传送、DLSS、音频、物品、天赋、剧情推进、过场、性能采集等），可搜索、按分区筛选；会改写存档的项目须在三秒内再点一次确认 |
| **其他** | DualSense 自适应扳机可关闭；全部设置写入 `PBZDebugMenu.ini` |

## 安装

1. 先退出游戏。下载[发布包](https://github.com/Jersyi/cringe-blade-zero/releases/latest/download/CringeBladeZero.zip)并解压，双击 `Install.bat`：
   - 安装器从注册表找到游戏目录（Steam 卸载项或 Steam 库列表；找不到时可手动输入），并校验游戏版本。
   - 按 **1** 安装完整版，**2** 安装精简版（只关闭 DualSense 自适应扳机），**3** 卸载。
   - 复制后校验 SHA-256。目录里已有其他 MOD 的 `version.dll` 时先备份，卸载时还原。
   - 界面语言随 Windows 显示语言：中文系统显示中文，其他系统显示英文。

   也可以手动安装：把 `version.dll` 复制到

   ```text
   <PBZ 安装目录>\platform\steam\version.dll
   ```

2. 启动游戏。进入游戏时屏幕上方出现“豪之刃”横幅，按 **F10** 打开面板。

3. 可选：把补充包 `PBZDebugSupplement.pak`、`.utoc`、`.ucas` 放进 `<PBZ 安装目录>\ProjectPBZ\Content\Paks\~mods\`，面板会多出“调试补充”页。补充包由更新前的游戏文件生成，不随发布包提供；没有它时 DLL 照常工作。

> MOD 只在匹配的 `PBZ.exe` 版本上启用（启动时校验 SHA-256）。版本不符时（通常是游戏更新后）不读写任何游戏数据，只保留关闭 DualSense 自适应扳机，并在画面上方提示“豪之刃 未适配当前游戏”。当前版本对应 2026-11-04 更新后的游戏。日志写在 DLL 旁的 `PBZDebugMenu.log`。

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

发布包内含完整版与精简版 `version.dll`、一键安装 `Install.bat` 与 `install.ps1`、安装说明 `README.txt`（含默认快捷键）和更新日志 `CHANGELOG.md`。

| 目标 | 产物 |
|---|---|
| `version` | `build/Release/version.dll`，完整 MOD |
| `version_triggers` | `build/triggers_only/version.dll`，只关闭自适应扳机的精简版 |
| `proxy_test` · `hair_controls_test` · `game_thread_test` · `pad_triggers_test` · `input_focus_test` · `npc_pose_transfer_test` | 测试（`input_focus_test` 用假 UnrealWindow 线程验证面板焦点、鼠标捕获与按键投递，以及三套快捷键的录制、触发、旧 INI 迁移和设备识别；`npc_pose_transfer_test` 用合成骨架验证 NPC 姿势重定向、双手握持、武器缩放与坐骑代理） |

编译选项：`/W4 /WX /utf-8`，静态 CRT。

## 默认快捷键

| 操作 | 键盘 | Xbox | PlayStation |
|---|---|---|---|
| 释放投技 | `G` | `Y` + `B` | `△` + `○` |
| 施放 NPC 技能 | `X` | `L3` + `R3` | `L3` + `R3` |
| 切换武器预设 | `Z` | `LB` + `L3` | 触控板左右滑动 |
| 提升 / 降低挡位 | `=` / `-` | — | — |
| 显示 / 隐藏面板 | `F10` | — | — |
| 隐藏面板 · 切换页面 | `Esc` · `Ctrl` + `Tab` | — | — |
| 毛发页 · 蒙皮 / Groom 切换 | `F7` · `F8` | — | — |
| 强制 Groom · 套装解锁 | `F6` · `Shift` + `F6` | — | — |
| GPU 包围盒 · 自适应扳机 | `F9` · `Shift` + `F9` | — | — |

键盘默认键避开了游戏自己的键位（WASD、E、F、Q、R、T、V、空格、Shift、Ctrl、1、2 等）。手柄的每个键游戏都在用，所以手柄快捷键都是两键同时按（220 毫秒内）。

每个功能有键盘、Xbox、PlayStation 三套绑定，都可在面板“快捷键”页的三列里重新录制：

- 键盘那套始终有效。
- 手柄只用当前手柄那一套。“手柄类型”默认“自动”，按最近按下的手柄识别（经 Steam Input / DS4Windows 映射的 DualSense 也识别为 PlayStation），也可固定为 Xbox 或 PlayStation。
- 屏幕上的提示跟随最近使用的设备：按过键盘或鼠标就显示键盘键，按过手柄就显示该手柄的按键。

快捷键也可以直接在 `PBZDebugMenu.ini` 的 `[Hotkeys]` 里改，重启游戏生效：

- 每个功能三项：`GrabRelease=kb:G`（键盘）、`GrabRelease.Xbox=xb:Y+B`、`GrabRelease.PS=ps:Triangle+Circle`。首次启动写入全部默认值。
- 值可以不带 `kb:` / `xb:` / `ps:` 前缀，空格随意，例如 `ToggleHub=Ctrl + F10`、`NpcSkill.Xbox=LB + RB`；留空表示不绑定。
- 打开面板的 `ToggleHub` 留空或写错时仍用 F10。
- 旧版每个功能只有一项；升级后自动迁移：原手柄组合键同时用于 Xbox 和 PlayStation 两套，键盘用新默认键。

## 设置文件

`PBZDebugMenu.ini` 与 DLL 放在一起，修改时才写盘（`[Hotkeys]` 例外：首次启动写入全部快捷键的默认值）：

| 节 | 内容 |
|---|---|
| `[UI]` | 界面语言 |
| `[Hub]` | 打开面板时暂停游戏 |
| `[Hotkeys]` | 快捷键：每个功能 `<功能>`（键盘）、`<功能>.Xbox`、`<功能>.PS` 三项；`PadGlyphs` 为手柄类型（`auto` / `xbox` / `ps`）；`ToggleHub` 为打开面板的键（默认 `kb:F10`） |
| `[Gear]` | 当前挡位 |
| `[Grab]` · `[GrabExtras]` · `[GrabDamage]` | 投技招式与开关、换武器 / 伤害模式 / 无视无敌、各招实测伤害 |
| `[NpcSkill]` | NPC 技能伤害倍率 `DamageMultiplier`（1–10，默认 3） |
| `[AirSkills]` | 空中施放、近敌 JC、缓落与奖励 |
| `[WeaponPresets]` | 两套武器预设与当前套 |

## 项目结构

```text
src/
├─ core/          DLL 入口与转发（proxy.cpp、forwarders.asm、version.def）、游戏线程钩子、UObject 反射、设置文件
├─ ui/            面板、主题、动画、输入焦点与鼠标捕获、双语、快捷键、横幅、挡位胶囊、面板暂停、技能提示
├─ boss/          Boss 连战、Boss AI、入场保护与存档保护
├─ grab/          投技：原生成对激活、状态租约、Boss 武器与姿态代理、伤害
├─ air/           空中技能、空中闪避、JC 与奖励
├─ weapons/       武器预设与游戏内武器图标
├─ character/     角色替换、NPC 外观重定向（姿势传递、武器适配、碰撞体、技能、坐骑代理）
├─ hair/          毛发显示与 Groom
├─ supplement/    可选补充包：运行时挂载、调用原版调试蓝图、调试补充页
├─ pad/           DualSense 自适应扳机
├─ generated/     从游戏数据生成的表（投技目录、武器、骨骼、NPC 档案 / 技能 / 坐骑、UI 资源、导出表）
└─ triggers_only/ 精简版 version.dll
tests/            proxy / 毛发 / 游戏线程 / 扳机 / 输入焦点 / NPC 姿势 / 补充页目录测试
installer/        一键安装（Install.bat 启动 install.ps1；打包时填入匹配的 PBZ.exe 大小与 SHA-256）
docs/             README 图片
```

每个 `src/` 子目录都是头文件搜索路径，源码里直接写 `#include "ue_runtime.h"`。

## 更新日志

见 [CHANGELOG.md](CHANGELOG.md)。

## 说明

- 仅用于单机游戏，与游戏开发商无关。
- `src/generated/` 内的图标、字体子集与数据表取自游戏文件，版权归原权利人所有。
