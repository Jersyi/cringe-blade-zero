# 游戏更新后的适配流程

这份指南面向维护 DLL 的开发者。适配需要逐项核对 EXE 身份、固定地址、自检字节、引擎布局和资源依赖。只替换 EXE 的 SHA256 不足以完成适配。

## 理解版本保护

[proxy.cpp](../src/core/proxy.cpp) 先检查 PE 时间戳、映像大小、入口点、文件大小，再校验 SHA256。未知版本会记录 `UNSUPPORTED_BUILD; no data writes`，停止游戏数据功能，保留关闭自适应扳机的降级路径并显示未适配提示。

当前 Steam / WeGame 共用固定 RVA 和引擎布局。[game_build.cpp](../src/core/game_build.cpp) 只为已核对的保护器跳转字节提供差异表，`GameBuild::CodeMatches` 根据已经验证的运行版本选择字节。游戏下一次更新未必仍满足这些条件。

| 检查层 | 负责位置 | 更新时的工作 |
|---|---|---|
| EXE 身份 | `src/core/proxy.cpp` | 更新五项版本事实 |
| 固定 RVA、自检字节 | `src/`、`tests/` | 定位、反汇编对照、重读字节 |
| 两端字节差异 | `src/core/game_build.cpp` | 重新验证同 RVA，生成 WeGame 差异表 |
| UObject / FProperty、虚表和原生参数布局 | `src/core/ue_runtime.*` 及功能模块 | 手工检查布局与运行时校验，地址脚本不包办 |
| 反射名字、资源路径、技能数据 | 各模块及 `src/generated/` | 对比新资源与反射，必要时重跑生成器 |
| 安装器、EnvCheck | `build.ps1`、`src/env_check/known_exes.cmake` | 构建时从 proxy 生成，避免版本表漂移 |

## 准备公开适配工具

[scripts/update_port.py](../scripts/update_port.py) 从本项目内部工具整理而来，移除了固定游戏安装路径。以下锁定依赖使用 Python 3.12 或以上，本轮核对环境为 Python 3.12.7。

```powershell
python -m pip install -r .\scripts\requirements-port.txt
python .\scripts\update_port.py --help
```

脚本只读 EXE，不修改游戏文件。`snapshot`、`locate` 写本地 JSON；`apply`、`build wegame` 会改仓库源码。先建立工作分支并保留可回滚提交。它们不是事务式操作：失败后可能已经产生部分源码修改，必须检查 diff。

默认数据目录为忽略提交的 `local/data/`；可用 `--data-dir` 指向一次适配专用目录。整个流程始终使用同一目录。EXE、定位 JSON、反射导出和资源缓存都留在本地，不上传仓库。

## 1. 在更新前保存基线

保留一份当前已支持的 Steam EXE 和源码提交；更新到新游戏后不能用新 EXE 代替旧基线生成特征。下面路径为示例，请换成自己的只读备份路径，后续步骤沿用同一 PowerShell 会话中的变量。

```powershell
$oldSteamExe = 'D:\PBZPort\old-steam\PBZ.exe'
$newSteamExe = 'D:\PBZPort\new-steam\PBZ.exe'
$newWeGameExe = 'D:\PBZPort\new-wegame\PBZ.exe'
$portData = 'local\port-session'
python .\scripts\update_port.py snapshot --exe $oldSteamExe --data-dir $portData
if ($LASTEXITCODE -ne 0) { throw 'Snapshot failed' }
```

输出 `exe_signatures.json`，记录 EXE 身份和每个地址的定位证据。`snapshot` 打印的缺少定位器数量也要检查，不能只看退出码。新增固定地址后要刷新基线，旧文档里的“71 个地址”只是历史统计，当前数量由扫描器决定。

扫描器识别 `Base()` / RVA 常量、`namespace layout`、`// exe-pins` 表和 `// exe-bytes: <常量名或地址>` 标记。`audit` 会报告未分类的疑似地址。普通标志和大小常量需人工确认后纳入排除规则，不能用排除规则隐藏真正的地址。

## 2. 定位新 Steam EXE

```powershell
python .\scripts\update_port.py locate --exe $newSteamExe --data-dir $portData
if ($LASTEXITCODE -ne 0) { throw 'Relocation failed; inspect its report' }
```

输出 `exe_relocated.json`。函数本体、RIP 相对引用、虚表槽、原生注册名、函数内偏移等证据互相校验；位移和分支目标按规则打通配。单一证据和邻近全局推断需要额外人工验证。

检查 `missing`、`MISSING`、`DISAGREE` 和每行 `why`。全部定位且证据一致才继续。不要把 `--force` 当成正式发布路径，也不要承诺固定时间内能完成未知版本的适配。

## 3. 应用并审查源码差异

```powershell
python .\scripts\update_port.py apply --data-dir $portData
if ($LASTEXITCODE -ne 0) { throw 'Apply failed; inspect working tree' }
git diff --stat
git diff -- src/core/proxy.cpp src/core/game_build.cpp
```

`apply` 使用定位记录中保存的 EXE 路径；保持该备份文件不变。它更新 proxy 的版本事实、固定 RVA，并在代码语义未发生可见变化时重读自检字节。

遇到 `CODE CHANGED, left`，脚本保留旧字节让功能校验失败；即使退出码为零也仍需人工处理。对照调用约定、参数、函数行为和布局后再修复，不能直接用新字节替换以消除报错。

完成源码核查后，保存旧 JSON 的归档，再为下一轮更新生成新基线：

```powershell
python .\scripts\update_port.py snapshot --refresh --exe $newSteamExe --data-dir $portData
if ($LASTEXITCODE -ne 0) { throw 'New snapshot failed' }
```

## 4. 核对 WeGame，决定能否继续共用 DLL

```powershell
python .\scripts\update_port.py build wegame --exe $newWeGameExe --primary-exe $newSteamExe --data-dir $portData
if ($LASTEXITCODE -ne 0) { throw 'WeGame adaptation incomplete; inspect source changes' }
python .\scripts\update_port.py audit --exe $newSteamExe --data-dir $portData
if ($LASTEXITCODE -ne 0) { throw 'Steam audit failed' }
python .\scripts\update_port.py audit --exe $newWeGameExe --data-dir $portData
if ($LASTEXITCODE -ne 0) { throw 'WeGame audit failed' }
```

`--primary-exe` 指定用于解码对照的 Steam EXE，省略时读取 snapshot 保存的路径；工具会先检查它同时匹配源码和基线的 SHA256。`build wegame` 要求所有固定地址仍在相同 RVA，只把符合保护器跳转规则的字节差异写入 `game_build.cpp`，并更新 `kWeGame*` 身份常量。地址不同则拒绝；发现更多代码差异时也会报错，且可能已经写入部分结果。

两端 `audit` 都应为 `0 problem(s)`。若某端地址或布局已分叉，应增加经过验证的按版本地址/布局实现，或暂不声明支持该端，不能只改哈希强行沿用。当前统一包方案只适用于已经满足共用条件的两个构建。

## 5. 检查资源和反射

重建新游戏的 IoStore 文件索引、包依赖、ScriptObjects 和反射缓存。包索引位置、导出位置、属性偏移和技能时序缓存不能跨版本直接沿用。

对比资源路径、类/函数名字、属性类型、参数长度、骨骼与挂点、武器组件、技能时间轴和过场表项。通过各表的拥有者生成器更新 `src/generated/`，保持 NPC 的并行表行数及编号一致；常规构建不需要重新生成这些表。

历史生成器依赖维护者本地资源分析工程，目前没有全部公开。本仓库包含已生成的数据和运行时代码，不包含一套从任意新版游戏自动生成全部表的流水线。独立补充包还需重新核对依赖闭包与原生函数是否存在，见[独立 pak 流程](PAK_WORKFLOW.md)。

## 6. 编译与验证后发布

按照[编译指南](BUILD.md)构建，并运行版本识别、环境检查、姿势、技能、过场、JC 和存档回归。游戏关闭时安装，保留旧 DLL 和个人配置；验证期间不要覆盖玩家的 `default.combo`。

实际运行应分别记录 Steam 和 WeGame 的 EXE SHA256、DLL SHA256、场景及结果。至少覆盖启动与返回标题、F10 面板、切图、外观切换与恢复、武器、代表性 BOSS 技能、JC、过场播放和模型接管、连招读写。不要以离线 `audit` 代替命中、特效、动画和焦点行为的实机验证。

更新 CHANGELOG，写明支持范围、离线/实机验证边界和旧存档兼容性。两端仍共用 DLL 时只制作一个 ZIP。

## 报错处理表

| 报告 | 含义与处理 |
|---|---|
| `MISSING` / `DISAGREE` | 定位缺失或证据冲突；对照新旧 EXE，不执行强制发布 |
| `CODE CHANGED, left` | 可变位移以外的代码改变；人工重审调用契约 |
| `NO SIGNATURE` / `SNAPSHOT` | 基线缺项或版本不对应；在正确 EXE 上补齐特征 |
| `UNCLASSIFIED` | 新增常量未被扫描规则覆盖；判断它是否确为地址 |
| `STALE ... row` | 两端差异表与当前基线不符；重新核对并生成 |
| `DIFFERS` / `NOT IN EXE` | 固定或动态代码自检不匹配；检查函数与字节规则 |
| 运行时布局校验失败 | 重审 UE 类型、字段和虚表布局；定位脚本不能修复 |

本文命令参数已按公开脚本核对。没有新游戏版本时，不把原地核对写成“已完成下一版适配”。

## 本次工具核对记录

2026-11-09 在隔离源码副本上核对当前已支持 EXE：Steam 73 项原地定位全部找回，`apply` 没有改变源码内容；WeGame 73 项保持相同 RVA，生成 4 条保护器字节差异、0 项额外代码变化。两端 `audit` 均为 `0 problem(s)`。拒绝错误 Steam 对照 EXE 的前置校验也已检查。

这是当前版本上的工具回归，不是对未知更新的成功承诺；本轮没有启动游戏。
