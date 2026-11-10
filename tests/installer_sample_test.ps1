$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('pbz_sample_' + [Guid]::NewGuid().ToString('N'))
$Here = Join-Path $testRoot 'package'
$game = Join-Path $testRoot 'game [fixture]'
New-Item -ItemType Directory -Path (Join-Path $Here 'samples'), (Join-Path $game 'combos') -Force | Out-Null
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile((Join-Path $root 'installer\install.ps1'), [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw ($errors | Out-String) }
foreach ($name in @('T', 'Get-Sha', 'Install-SampleCombo', 'Install-Dll')) {
    $function = $ast.Find({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name }, $true)
    if (-not $function) { throw "Missing function $name" }
    Invoke-Expression $function.Extent.Text
}
$Zh = $false; $checks = 0
function Check([bool]$ok, [string]$why) { $script:checks++; if (-not $ok) { throw $why } }
function Wait-GameClosed { return $true }
function Test-OurDll { return $true }
function Test-AccessDenied { return $false }
$source = Join-Path $Here 'samples\sampleCombo.combo'
Copy-Item -LiteralPath (Join-Path $root 'samples\sampleCombo.combo') -Destination $source
$default = Join-Path $game 'combos\default.combo'
$ini = Join-Path $game 'PBZDebugMenu.ini'
[IO.File]::WriteAllText($default, 'player default sentinel')
[IO.File]::WriteAllText($ini, '[NpcCombo]' + "`r`nFile=default")
$defaultSha = Get-Sha $default; $iniSha = Get-Sha $ini; $sha = Get-Sha $source
$first = Install-SampleCombo $game
Check ((Split-Path $first -Leaf) -eq 'sampleCombo.combo' -and (Get-Sha $first) -eq $sha) 'fresh installation'
Check ((Install-SampleCombo $game) -eq $first) 'identical installation is idempotent'
[IO.File]::WriteAllText($first, 'player edited example')
$editedSha = Get-Sha $first
$second = Install-SampleCombo $game
Check ($second -ne $first -and (Get-Sha $first) -eq $editedSha -and (Get-Sha $second) -eq $sha) 'edited example preserved'
Check ((Install-SampleCombo $game) -eq $second) 'existing bundled version reused'
[IO.File]::WriteAllText($second, 'edited bundled example')
$third = Install-SampleCombo $game
Check ($third -ne $second -and (Get-Sha $third) -eq $sha) 'secondary name collision preserved'
$FullDll = Join-Path $Here 'version.dll'; $TriggersDll = Join-Path $Here 'triggers_only.dll'
[IO.File]::WriteAllText($FullDll, 'test full dll'); [IO.File]::WriteAllText($TriggersDll, 'test triggers dll')
$target = Join-Path $game 'version.dll'
Check (Install-Dll '1' $target) 'full installation includes sample successfully'
Check ((Get-Sha $target) -eq (Get-Sha $FullDll)) 'full dll copied'
Check (Install-Dll '2' $target) 'triggers-only install still works'
Check ((Get-Sha $target) -eq (Get-Sha $TriggersDll)) 'triggers dll copied'
$priorSha = Get-Sha $target
Remove-Item -LiteralPath $source
Check (-not (Install-Dll '1' $target)) 'missing sample rejected before modifying DLL'
Check ((Get-Sha $target) -eq $priorSha) 'preflight preserves installed DLL'
Check ((Get-Sha $default) -eq $defaultSha -and (Get-Sha $ini) -eq $iniSha) 'default and active-file setting untouched'
@{ checks = $checks; failures = 0; isolated_root = $testRoot; runtime = $false } | ConvertTo-Json -Compress
