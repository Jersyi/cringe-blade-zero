# 独立 pak 的制作、挂载与使用

本项目通过 `version.dll` 在游戏线程主动挂载独立资源容器。这里的“独立 pak”是一组同名的 `.pak`、`.utoc`、`.ucas`：`.pak` 是挂载入口，实际 cooked 资源主要在 IoStore 的另外两个文件中。

当前发布代码接入可选的 `PBZDebugSupplement` 调试补充包和 `PBZSoftBones` 胸臀骨骼补充包。胸臀包放在 `version.dll` 旁的 `PBZSoftBones` 目录，见 [安装说明](SOFT_BONES.md)。剑心及女性化主角外观的专用挂载入口已移除；当前 DLL 不会自动加载任意名字的新 MOD。下面同时说明已有补充包的使用方法，以及开发者制作新容器后需要补上的接入工作。

## 解包密钥

维护者提供的 PBZ 资源解包 AES-256 密钥（32 字节）：

```text
0x4bc8a51e5381325073f2bc7d4e3089cd557c4e9b7d453901c2c795518867e01d
```

在读包工具的 AES Key 配置中填写上述值；工具若要求纯十六进制，去掉开头的 `0x`。Python 读取器需要字节串时，使用 `bytes.fromhex(key.removeprefix('0x'))` 转换，不能直接传入 UTF-8 文本字节。

该密钥用于读取加密的游戏资源输入。本项目制作的未加密独立容器无需使用它；空 pak 制作工具也不读取密钥。本次仅核对格式和长度，未重新解包验证新版资源；游戏更新后仍需确认实际容器是否沿用此密钥。

## 已有补充包如何使用

先退出游戏，将同一次构建产生的三个文件放在同一目录：

```text
<游戏根目录>/ProjectPBZ/Content/Paks/~mods/PBZDebugSupplement/
    PBZDebugSupplement.pak
    PBZDebugSupplement.utoc
    PBZDebugSupplement.ucas
```

安装匹配游戏版本的完整版 `version.dll`，然后由玩家启动游戏。检测到完整三件套后，F10 面板会注册“调试补充”页。进入游戏后第一次游戏线程 tick 执行挂载，具体蓝图在请求时加载。

搜索位置以 [debug_supplement.cpp](../src/supplement/debug_supplement.cpp) 的 `Initialize` 为准：从 `PBZ.exe` 目录计算 `../../ProjectPBZ/Content/Paks/`，搜索该目录及向下三层子目录。Steam 的标准结构符合此相对路径；不同 WeGame 安装布局不能只照抄 Steam 的盘符，应以 `SUPPLEMENT ... under` 日志显示的实际搜索目录为准。当前实现没有按平台另行发现 Paks 目录。

标准发布 ZIP 和源码仓库不包含补充资源包。读者需要自行准备兼容且可使用的资源；仅下载本仓库不会得到旧版游戏资产。

## 为什么需要 DLL 主动挂载

本项目在已适配 PBZ 版本上的观察是：游戏启动挂载流程会跳过额外 pak，单纯改成 `pakchunk...`、放进 `~mods` 或 `LogicMods` 没有让这些包自动生效。运行时使用游戏现有的挂载函数，不修改游戏 EXE 或引擎 DLL。

```mermaid
flowchart LR
    A[发现同名三件套] --> B[已支持 EXE 与函数字节检查]
    B --> C[游戏线程找到 PakFile 平台层]
    C --> D[Mount 空 pak]
    D --> E[引擎挂载同名 utoc 与 ucas]
    E --> F[按原始包路径加载资产]
```

实现依次调用 `FPlatformFileManager::Get()`、`FindPlatformFile(L"PakFile")` 和 `FPakPlatformFile::Mount`。本项目包装的 `PakMountArgs` 大小为 `0x20`，`order`、`path`、`flags`、`load_index` 偏移分别为 `0x08 / 0x10 / 0x18 / 0x1c`；`order=4`、`load_index=true`。这些是当前适配版本的 ABI，不是跨 UE 版本稳定接口。

`EngineCodeMatches` 通过 `GameBuild::CodeMatches` 检查函数字节。`MountPak` 记录挂载结果、IoStore 状态码和系统错误。只在游戏线程调用，并由外层异常保护处理失败；同一进程只尝试一次，文件或代码修正后重启再测。挂载成功不等于包能解析、蓝图能加载或功能能执行。

## 制作路线 A：Cook 自己的资源

适合制作自己拥有的模型、蓝图和配套资源。需要独立的 UE 工程以及与目标游戏兼容的序列化、插件和依赖；本仓库不含示例美术工程。

历史自制资源流程使用 UE 5.7 commandlet，补充容器布局分析使用 UE 5.8 源码。这个组合是项目中的具体经验，不保证任意 UE 5.7/5.8 资源均可加载。必须核对目标游戏的自定义版本、类、材质和骨骼要求。

### 1. 保持包路径和依赖一致

以 `ProjectPBZ` 为工程名，将示例 MOD 放在 `Content/Mods/Example/`，运行时包路径为 `/Game/Mods/Example/...`。需要引用游戏已有资源时，制作工程中的占位对象必须保持对应包名、对象名和兼容类型；打包时排除这些占位对象，运行时才会使用游戏原有资源。

尽量使用游戏已有材质及兼容的材质实例。新增 shader、原生类或插件需要完整处理自己的依赖，不能靠一个空 pak 自动解决。Cook 时保留带属性名的序列化方式，不把另一套游戏类布局下的 unversioned 属性直接当作兼容资源。

制作工程的 `Config/DefaultGame.ini` 可按原流程设置：

```ini
[/Script/UnrealEd.ProjectPackagingSettings]
bUseIoStore=True
UsePakFile=True
bShareMaterialShaderCode=True
bSharedMaterialNativeLibraries=True
bUseZenStore=False
```

### 2. Cook 指定目录

以下 PowerShell 示例在仓库根目录执行；先替换本机 UE 与自制工程路径。它会运行 Unreal commandlet，只有准备好工程并打算实际 Cook 时才执行。

```powershell
$editor = 'D:\UE_5.7\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$project = 'D:\PBZMods\ProjectPBZ'
$uproject = Join-Path $project 'ProjectPBZ.uproject'
$modName = 'PBZExample'
$cooked = Join-Path $project 'Saved\Cooked\Windows'
$out = Join-Path $PWD 'local\pak-example'
New-Item -ItemType Directory -Force $out | Out-Null
& $editor $uproject '-run=cook' '-targetplatform=Windows' "-cookdir=$project\Content\Mods\Example" '-nullrhi' '-unattended' '-nosplash' "-abslog=$out\cook.log"
if ($LASTEXITCODE -ne 0) { throw 'Cook failed' }
```

确认 `Saved/Cooked/Windows/ProjectPBZ/Content/Mods/Example/` 和 `ProjectPBZ/Metadata/` 已生成。Cook 成功后检查日志中缺失类、导入和 shader 的警告，不要只检查进程退出码。

### 3. 只把 MOD 自有资源加入 IoStore

以下代码沿用上一步变量。Response 文件每行包含真实 cooked 文件和虚拟目标路径；保持 `../../../ProjectPBZ/Content/...` 路径，不要改成电脑绝对路径作为包名。

```powershell
$modDir = Join-Path $cooked 'ProjectPBZ\Content\Mods\Example'
$files = @(Get-ChildItem -LiteralPath $modDir -Recurse -File |
    Where-Object { $_.Extension -in '.uasset', '.uexp', '.ubulk', '.uptnl', '.umap' } |
    Sort-Object FullName)
if ($files.Count -eq 0) { throw 'No cooked MOD files' }
$utf8 = New-Object System.Text.UTF8Encoding($false)
$response = Join-Path $out "$modName.rsp"
$lines = foreach ($file in $files) {
    $relative = $file.FullName.Substring($cooked.Length + 1).Replace('\', '/')
    '"' + $file.FullName + '" "../../../' + $relative + '"'
}
[IO.File]::WriteAllLines($response, [string[]]$lines, $utf8)
$commands = Join-Path $out 'commands.txt'
$command = '-Output="' + $out + '\' + $modName + '.utoc" -ContainerName=' + $modName + ' -ResponseFile="' + $response + '"'
[IO.File]::WriteAllText($commands, $command + "`r`n", $utf8)
$metadata = Join-Path $cooked 'ProjectPBZ\Metadata'
& $editor $uproject '-run=iostore' "-CreateGlobalContainer=$out\global.utoc" "-CookedDirectory=$cooked" "-PackageStoreManifest=$metadata\packagestore.manifest" "-ScriptObjects=$metadata\scriptobjects.bin" "-Commands=$commands" '-TargetPlatform=Windows' '-unattended' '-nosplash' "-abslog=$out\iostore.log"
if ($LASTEXITCODE -ne 0) { throw 'IoStore creation failed' }
```

产物应包括 `PBZExample.utoc/.ucas`。临时生成的 `global.*` 属于制作工程，不能随 MOD 覆盖或替换游戏的 `global.utoc`。最终交付只挑选同名三件套；不要把整个输出目录直接复制到游戏里。

### 4. 创建空 pak 挂载入口

[make_mount_pak.py](../scripts/make_mount_pak.py) 只依赖 Python 标准库，生成本项目使用的 313 字节空 v12 pak：

```powershell
python .\scripts\make_mount_pak.py (Join-Path $out "$modName.pak")
if ($LASTEXITCODE -ne 0) { throw 'Mount pak creation failed' }
```

工具拒绝覆盖已有文件，避免误替换承载资产的 pak。它包含空完整目录索引和对应 SHA1，挂载点为 `../../../`；不包含模型、蓝图、密钥或 Cook 数据。单独生成 `.pak` 不能代替制作 `.utoc/.ucas`。

### 5. 接入 DLL

`PBZExample.*` 是制作示例，当前发布 DLL 没有它的注册项。开发者需参照 `src/supplement/debug_supplement.cpp` 增加自己容器的发现、挂载状态和游戏线程入口，先成功挂载，再加载该容器中的对象。

当前 `FindContainer` / `MountPak` 在补充模块的匿名命名空间内，不是可供其他文件直接调用的公共 API。添加新模块时，应将通用挂载能力明确抽成共享接口或在模块内接入，并在 `src/core/proxy.cpp` 的游戏线程分发中安排调用顺序。

不要只把 `kContainer` 改名：现有补充模块还固定加载 `BP_CollectHelp_C`，并依赖补充目录、反射函数与 UI。新模型需要自己的对象加载、角色注册和清理路径；新独立功能也要实现自己的入口。

## 制作路线 B：从兼容 cooked 包组成补充容器

这是 `PBZDebugSupplement` 的实际制作方式，适合已经拥有可使用的 cooked 资源，并能核对其与当前游戏的依赖。它与“从源资产重新 Cook”不同，不把旧主包整体覆盖到新游戏上。

制作时以 `/Game/PBZContent/UI/Help/BP_CollectHelp` 为根计算依赖闭包：当前游戏仍存在的包继续引用当前版本，只收集缺少的包。逐项检查脚本导入是否存在于新 `global.utoc` 的 ScriptObjects，以及保留包的 public export hash 是否能满足旧资源导入；任何缺项都应先处理。

内部流水线的阶段和格式如下，资源读包器、旧游戏包和本地依赖数据不随仓库发布：

| 阶段 | 输入 | 输出和约束 |
|---|---|---|
| 依赖闭包 | 根包、新旧资源索引、导入/导出表 | 缺失包集合；脚本导入与 public export 检查无问题 |
| 收集 chunks | 闭包 PackageId | 保留关联 chunk ID 和解压后的原始内容，不能只拷 ExportBundle 而漏 BulkData |
| 重建 PackageStore | 包导入列表 | 重建 `FFilePackageStoreEntry` 相对偏移，保持 PackageId 对应 |
| 重建 ContainerHeader | 新容器 ID、包 ID、store entries | header chunk 类型 6；包 ID 按制作器约定排序 |
| 写 IoStore | chunks、TOC 元数据 | 输出 `.utoc/.ucas`，使用下述经过验证的格式 |
| 写挂载入口 | 空目录索引 | 输出同名 `.pak`，可使用公开的 `make_mount_pak.py` |
| 回读校验 | 新容器与源 chunks | 每个包 chunk 内容一致，块对齐、边界、依赖正确 |

原补充包制作器采用未压缩、未加密、未签名、无目录索引、无 perfect hash 的布局。具体约定是 TOC v8、144 字节 header、12 字节 chunk ID、10 字节 offset/length、12 字节压缩块记录、24 字节 chunk meta；逻辑块大小 `0x10000`，磁盘上每块补齐到 16 字节。Offset/length 两个 5 字节字段使用大端，块记录中的 offset/size 使用小端。

原 chunk 的 20 字节 IoHash 保留，meta flags 清零；没有内容变化时才能复用其 hash。新 ContainerHeader 使用版本 5、magic `0x496f436e`，容器 ID 与 TOC 一致。项目原制作器用容器名小写的 SHA1 前 8 字节按小端得到 ID；这是本项目的生成约定，不是游戏包 PackageId 的通用计算方式。

原制作器对 shader map hashes 非空的 store entry 直接拒绝。若换成含新增材质 shader 的资源，需扩展并验证完整 shader 依赖，不能继续套用这个受限布局。加密输入使用上文记录的解包密钥，解码依赖由本地环境提供；脚本不内置密钥或解码库。

历史补充包制作记录为 9 个 package、10 个 chunk、13 个块，`.pak` 为 313 字节、`.utoc` 为 760 字节、`.ucas` 为 269792 字节。这些是那一次闭包的核对值，不是新 MOD 必须满足的数量。完整二进制重打包器未在本仓库公开，以上是重实现和审计所需的制作约束；不要把空 pak 工具误认为完整资源提取器或 IoStore 打包器。

## 验证挂载和调用

首先离线检查三件套同名、文件完整、chunk ID 唯一、块范围有效、依赖可解析，并记录 SHA256。对于路线 B，重新读回每个包 chunk，与原始解压内容逐字节比较。

准备实机验证时，先退出游戏再安装。启动后查看 DLL 旁的 `PBZDebugMenu.log`：

| 日志或现象 | 检查方向 |
|---|---|
| `UNSUPPORTED_BUILD` | EXE 尚未适配；先完成版本检查 |
| `SUPPLEMENT container absent under ...` | 实际搜索根、目录深度、同名三件套是否齐全 |
| `mount skipped: engine code differs` | 挂载函数地址或自检字节不匹配；按新版本重新验证 |
| `mount skipped: no PakFile platform layer` | 平台文件层或调用时机不符合预期 |
| `SUPPLEMENT mount ... ok=0` | 容器格式、依赖、IoStore 错误码与系统错误 |
| `class not loaded` | 包路径、对象 `_C` 后缀、PackageStore 导入或序列化不兼容 |
| 单项 `missing functions` / `removed by the game` | 原生能力已删除或名字变化；补资源不能恢复已删的原生实现 |
| 过场只显示 `dispatched` | 仅代表请求派发；还要看到过场状态确认，不能当成已播放 |

维护工具可在游戏运行且允许发送请求时向 `PBZDebugMenu.command` 写入单行 `supplement status`、`supplement list`、`supplement load`、`supplement probe` 或 `supplement reflect`。文件由 DLL 消费并删除；这不是游戏控制台命令，也不应在玩家操作期间自动注入。

删除或替换已挂载资源前先退出游戏；当前模块没有热卸载。实机验证应覆盖资源加载、功能调用、切图、退出与重复启动。过场还需核对表项、角色/节点索引及世界暂停状态。

## 当前验证边界

2026-11-09 整理时，公开工具生成的 313 字节空 pak 已与历史可用挂载入口逐字节核对一致，已有文件覆盖与错误扩展名也被拒绝。Cook/IoStore 示例通过了 PowerShell 语法及模拟程序的参数、响应文件路径检查；本轮没有启动 Unreal 或游戏重新 Cook、挂载。文档整理不意味着重新 Cook 了任意新资源，也不意味着恢复了已移除的 MOD 自动加载入口。新容器及新游戏版本仍需独立验证。

相关文档：[编译与发布](BUILD.md)、[游戏更新适配](GAME_UPDATES.md)。
