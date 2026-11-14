# Builds every target (Release). -Package also writes the player ZIP to dist\.
param([switch]$Package)
$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
$buildRoot = Join-Path $projectRoot 'build'
& cmake -S $projectRoot -B $buildRoot -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
& cmake --build $buildRoot --config Release --parallel 2
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
if (-not $Package) { return }

$commit = (& git -C $projectRoot rev-parse --short HEAD 2>$null)
# Uncommitted changes to tracked files: the archive is not that commit's build.
if ($commit -and (& git -C $projectRoot status --porcelain --untracked-files=no 2>$null)) { $commit += '-modified' }
$name = 'CringeBladeZero_' + (Get-Date -Format 'yyyyMMdd') + $(if ($commit) { '_' + $commit } else { '' })
$readme = @"
豪之刃 · Cringe Blade Zero
《影之刃零》（Phantom Blade Zero）游戏内工具箱 MOD

安装
  一键安装：先退出游戏，双击 Install.bat，按 1 安装完整版、2 安装精简版、3 卸载。
    安装器从注册表找到游戏目录（找不到时可手动输入 PBZ.exe 所在文件夹），校验游戏版本，复制后再校验 DLL；
    目录里已有其他 MOD 的 version.dll 时先备份，卸载时还原。界面语言随系统显示语言（中文 / 英文）。
    完整版会同时安装作者的示例 combos\sampleCombo.combo；也可选 5 单独安装示例。
    如果已有改过的同名示例，随包版本另存为 sampleCombo_bundled_<校验码>.combo，保留原文件。
    启动后在“招式派生”的文件列表选择 sampleCombo；不会覆盖 default，也不自动更改当前派生文件。
  Steam / WeGame 共用同一个 version.dll，无需分别下载。
  手动安装：把 version.dll 复制到 PBZ.exe 所在的文件夹：
    Steam 版   <PBZ 安装目录>\platform\steam\version.dll
    WeGame 版  WeGame 游戏目录里 PBZ.exe 的旁边
  启动游戏，进入游戏后按 F10 打开面板。

环境检查与修复
  Install.bat 选 4，或直接双击 EnvCheck.exe（也可以把 PBZ.exe 拖到它上面）。检查 Windows、VC++ 运行库、Direct3D 12、
  MOD 用到的全部系统组件、手柄与字体；会拦下 MOD 的 Windows 设置（智能应用控制、Exploit protection 等）、安全软件和
  系统里的拦截记录；每个游戏安装：游戏版本、已装的 DLL、放错位置的副本、其他 MOD 加载器、写入权限、只读文件、
  派生文件和上次运行的日志；最后判断游戏到底有没有加载 MOD。发现问题时按编号修复：安装或更新 MOD、停用放错位置的
  副本、去掉下载标记、从微软官方下载并安装 VC++ 运行库（校验签名）、让 Steam 验证游戏文件、取消只读、移走损坏的
  派生文件、授予游戏目录写入权限、运行系统文件修复。需要管理员权限的修复会弹出确认。结果保存在 EnvCheck_report.txt。
  一切正常但 MOD 没有加载时：启动游戏停在标题画面，再运行 EnvCheck.exe，它会读出游戏实际加载的 version.dll。

默认快捷键（键盘 / Xbox / PlayStation；按最近使用的设备自动识别）
  释放投技          G       Y + B      三角 + 圈
  施放 NPC 技能     X       L3 + R3    L3 + R3
  切换武器预设      Z       LB + L3    触控板左右滑
  提升 / 降低挡位   = / -
  显示 / 隐藏面板   F10     View + Menu  Create + Options
  都可在面板“快捷键”页或 PBZDebugMenu.ini 的 [Hotkeys] 修改。
  面板里用手柄：十字键 / 左摇杆移动，A（叉）确认，B（圈）返回，LB / RB（L1 / R1）切换页面，LT / RT（L2 / R2）滚动。
  面板打开时游戏收不到手柄按键。

派生插件（可选）
  别人做的派生插件是 DLL 文件：放进 version.dll 旁的 PBZPlugins 文件夹，在“招式派生”页顶栏把“派生插件”打开，
  插件随即载入（之后再放进去的插件在下次启动游戏时载入）。不打开“派生插件”时 MOD 不载入任何插件。
  插件可以提供插件节点（按血量、距离、概率等条件走不同出口，或做一件事）和插件信号（自定义按键里多出的
  一种“按键”，例如只在空中轻攻击时按下）。
  随包的 PBZPlugins 文件夹里是三个示例插件（条件判断、武器动作信号、模块面板），需要时整个文件夹复制到 version.dll 旁边。
  插件要读写 MOD 的其他功能（外观、身材、毛发、Boss 连战……）时，要在插件列表里逐项授权，没授权的调用会被拒绝并写进日志。
  Lua 脚本：version.dll 旁的 PBZPlugins\scripts 文件夹里的 .lua 文件也是插件（和 DLL 在同一个插件列表里，同样要逐项授权），
  “派生插件”打开后读取；改完脚本在插件列表里点“重新读取脚本”即可，不用重启游戏。脚本里没有文件和系统命令，跑得太久或出错会被停用，
  状态显示在插件列表里。随包的 scripts 文件夹里是三个示例脚本。
  自己制作插件：SDK 文件夹里有接口头文件、示例源码、接口手册 PLUGIN_API.md 和上手指南 AGENT_GUIDE.md（模板、安装、授权、排错速查）。

精简版
  triggers_only\version.dll 只关闭 DualSense 自适应扳机，没有面板和其他功能。
  两个文件同名，只装其中一个。

卸载
  运行 Install.bat 选 3；或手动删除 PBZ.exe 旁边的 version.dll，以及同目录的 PBZDebugMenu.ini、PBZDebugMenu.log。

Donate（自愿）
  MOD 完全免费，不打赏也可以使用全部功能。喜欢的话可以在爱发电支持作者：https://afdian.com/a/Jersyi1
  游戏里面板最底部的 Donate 页有二维码，也能直接在浏览器中打开链接。

说明
  MOD 只在匹配的 PBZ.exe 版本上启用（Steam 版和 WeGame 版，启动时校验 SHA-256）。游戏更新后版本不符时，不读写游戏数据，
  只保留自适应扳机开关，并在画面上方提示“豪之刃 未适配当前游戏”。
  设置保存在 DLL 旁的 PBZDebugMenu.ini，日志写在 PBZDebugMenu.log。第一次启动（还没有 PBZDebugMenu.ini）时
  所有功能默认关闭，需要哪个就在面板里打开。
  这一版更新了什么、修复了什么，见同目录的 更新内容.txt。
  MOD 本身的代码以 MIT 许可发布（见 LICENSE）；第三方代码和取自游戏的资源保留各自的条款，见 THIRD_PARTY_NOTICES.md。
  仅用于单机游戏，与游戏开发商无关。

构建 $name
"@
$files = [ordered]@{
    "$name/version.dll"               = Join-Path $buildRoot 'Release\version.dll'
    "$name/triggers_only/version.dll" = Join-Path $buildRoot 'triggers_only\version.dll'
    "$name/EnvCheck.exe"              = Join-Path $buildRoot 'Release\EnvCheck.exe'
    "$name/更新内容.txt"              = Join-Path $projectRoot '更新内容.txt'  # for players; CHANGELOG.md stays in the repo
    "$name/LICENSE"                   = Join-Path $projectRoot 'LICENSE'  # MIT, this project's own code; third-party terms in THIRD_PARTY_NOTICES.md
    "$name/THIRD_PARTY_NOTICES.md"    = Join-Path $projectRoot 'THIRD_PARTY_NOTICES.md'
    "$name/licenses/UUEPBS-LICENSE-STATUS.txt" = Join-Path $projectRoot 'licenses\UUEPBS-LICENSE-STATUS.txt'
    "$name/docs/WEAPON_DERIVATIONS.md" = Join-Path $projectRoot 'docs\WEAPON_DERIVATIONS.md'
    "$name/docs/ANIMATION_PRESETS.md" = Join-Path $projectRoot 'docs\ANIMATION_PRESETS.md'
    "$name/docs/SOFT_BONES.md" = Join-Path $projectRoot 'docs\SOFT_BONES.md'
    "$name/docs/USER_MANUAL.md" = Join-Path $projectRoot 'docs\USER_MANUAL.md'
    "$name/docs/BUILD.md" = Join-Path $projectRoot 'docs\BUILD.md'
    "$name/docs/GAME_UPDATES.md" = Join-Path $projectRoot 'docs\GAME_UPDATES.md'
    "$name/docs/PAK_WORKFLOW.md" = Join-Path $projectRoot 'docs\PAK_WORKFLOW.md'
    # 派生插件: the three sample plugins ready to copy next to version.dll, and the SDK for plugin authors.
    "$name/PBZPlugins/pbz_conditions.dll" = Join-Path $buildRoot 'Release\pbz_conditions.dll'
    "$name/PBZPlugins/pbz_weapon_actions.dll" = Join-Path $buildRoot 'Release\pbz_weapon_actions.dll'
    "$name/PBZPlugins/pbz_weapon_actions.txt" = Join-Path $projectRoot 'sdk\samples\weapon_actions\pbz_weapon_actions.txt'
    "$name/PBZPlugins/pbz_mod_panel.dll" = Join-Path $buildRoot 'Release\pbz_mod_panel.dll'
    # Lua scripts (PBZPlugins\scripts, interface revision 3): the three samples, read once 派生插件 is on; the sources are in the SDK too.
    "$name/PBZPlugins/scripts/condition.lua" = Join-Path $projectRoot 'sdk\samples\lua\condition.lua'
    "$name/PBZPlugins/scripts/signal.lua" = Join-Path $projectRoot 'sdk\samples\lua\signal.lua'
    "$name/PBZPlugins/scripts/panel.lua" = Join-Path $projectRoot 'sdk\samples\lua\panel.lua'
    "$name/SDK/samples/lua/condition.lua" = Join-Path $projectRoot 'sdk\samples\lua\condition.lua'
    "$name/SDK/samples/lua/signal.lua" = Join-Path $projectRoot 'sdk\samples\lua\signal.lua'
    "$name/SDK/samples/lua/panel.lua" = Join-Path $projectRoot 'sdk\samples\lua\panel.lua'
    "$name/licenses/Lua-LICENSE.txt" = Join-Path $projectRoot 'licenses\Lua-LICENSE.txt'
    "$name/SDK/PLUGIN_API.md" = Join-Path $projectRoot 'docs\PLUGIN_API.md'
    "$name/SDK/AGENT_GUIDE.md" = Join-Path $projectRoot 'docs\AGENT_PLUGIN_GUIDE.md'  # how-to for agents (and people) writing plugins / scripts
    "$name/SDK/include/pbz_combo_api.h" = Join-Path $projectRoot 'sdk\include\pbz_combo_api.h'
    "$name/SDK/samples/CMakeLists.txt" = Join-Path $projectRoot 'sdk\samples\CMakeLists.txt'
    "$name/SDK/samples/conditions/pbz_conditions.cpp" = Join-Path $projectRoot 'sdk\samples\conditions\pbz_conditions.cpp'
    "$name/SDK/samples/weapon_actions/pbz_weapon_actions.cpp" = Join-Path $projectRoot 'sdk\samples\weapon_actions\pbz_weapon_actions.cpp'
    "$name/SDK/samples/weapon_actions/pbz_weapon_actions.txt" = Join-Path $projectRoot 'sdk\samples\weapon_actions\pbz_weapon_actions.txt'
    "$name/SDK/samples/mod_panel/pbz_mod_panel.cpp" = Join-Path $projectRoot 'sdk\samples\mod_panel\pbz_mod_panel.cpp'
    "$name/Install.bat"               = Join-Path $projectRoot 'installer\Install.bat'
    "$name/samples/sampleCombo.combo" = Join-Path $projectRoot 'samples\sampleCombo.combo'
    "$name/licenses/DearImGui-LICENSE.txt" = Join-Path $projectRoot 'licenses\DearImGui-LICENSE.txt'
}
# 身材 / 骨骼: the presets the installer puts next to version.dll (UUEPBS Presets\). Only UUEPBS's own three example presets
# (github.com/XTGMods/UUEPBS presets/, commit 7c54929) are in samples\UUEPBS Presets; the contributor's are kept out of the package.
$bodyPresets = Join-Path $projectRoot 'samples\UUEPBS Presets'
foreach ($preset in Get-ChildItem -LiteralPath $bodyPresets -Recurse -File -Filter '*.json') {
    $relative = $preset.FullName.Substring($bodyPresets.Length).TrimStart('\').Replace('\', '/')
    $files["$name/samples/UUEPBS Presets/$relative"] = $preset.FullName
}
# The manual is added when present; the first release build may precede PDF authoring.
foreach ($picture in Get-ChildItem -LiteralPath (Join-Path $projectRoot 'docs\manual_images') -Filter '*.png' -File) {
    $files["$name/docs/manual_images/$($picture.Name)"] = $picture.FullName
}
$manual = Join-Path $projectRoot 'output\pdf\豪之刃MOD功能与节点手册.pdf'
if (Test-Path -LiteralPath $manual -PathType Leaf) {
    $files["$name/豪之刃MOD功能与节点手册.pdf"] = $manual
    $readme += "`r`n完整功能、全部节点和示例使用说明：豪之刃MOD功能与节点手册.pdf（中文 PDF）。`r`n"
}
# The installer checks the game against the builds proxy.cpp accepts: Steam (kExeBytes, kSha256), WeGame (kWeGame...).
$proxy = Get-Content -LiteralPath (Join-Path $projectRoot 'src\core\proxy.cpp') -Raw
$exeBuilds = foreach ($build in @(@('Steam', ''), @('WeGame', 'WeGame'))) {
    $exeBytes = [regex]::Match($proxy, '\bk' + $build[1] + 'ExeBytes = (\d+);').Groups[1].Value
    $exeSha256 = [regex]::Match($proxy, '\bk' + $build[1] + 'Sha256 = "([0-9a-f]{64})"').Groups[1].Value
    if (-not $exeBytes -or -not $exeSha256) { throw "k$($build[1])ExeBytes / k$($build[1])Sha256 not found in proxy.cpp" }
    $build[0] + '|' + $exeBytes + '|' + $exeSha256
}
$installer = [IO.File]::ReadAllText((Join-Path $projectRoot 'installer\install.ps1')).Replace('__EXE_BUILDS__', ($exeBuilds -join ';'))
$distRoot = Join-Path $projectRoot 'dist'
New-Item -ItemType Directory -Path $distRoot -Force | Out-Null
$zipPath = Join-Path $distRoot "$name.zip"
Remove-Item -LiteralPath $zipPath -ErrorAction SilentlyContinue
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
# Entries are named explicitly so the archive always uses '/' separators.
$archive = [System.IO.Compression.ZipFile]::Open($zipPath, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($entry in $files.GetEnumerator()) {
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $entry.Value, $entry.Key, [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
    # Text entries: UTF-8 with BOM (Windows PowerShell 5.1 reads a script without one as ANSI) and CRLF.
    foreach ($text in @(@("$name/README.txt", $readme), @("$name/install.ps1", $installer))) {
        $writer = New-Object System.IO.StreamWriter($archive.CreateEntry($text[0]).Open(), (New-Object System.Text.UTF8Encoding($true)))
        try { $writer.Write($text[1].Replace("`r`n", "`n").Replace("`n", "`r`n")) } finally { $writer.Dispose() }
    }
} finally { $archive.Dispose() }
Write-Output $zipPath
