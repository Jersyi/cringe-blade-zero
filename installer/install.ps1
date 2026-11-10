# Cringe Blade Zero one-click setup (Windows PowerShell 5.1+), started by Install.bat.
# The game comes from the registry: the Steam uninstall entry's InstallLocation, the libraries listed in Steam's
# libraryfolders.vdf, then any uninstall entry named after the game (another store, such as WeGame); otherwise the
# player types the folder. The DLL goes next to PBZ.exe: <game>\platform\steam on Steam, the PBZ.exe folder elsewhere.
# Text follows the Windows display language: Chinese on a Chinese system, English everywhere else.
param([string]$Choice = '')
$ErrorActionPreference = 'Stop'

$Zh = $false
try { $Zh = [Globalization.CultureInfo]::CurrentUICulture.Name.StartsWith('zh') } catch {}
function T([string]$Chinese, [string]$English) { if ($Zh) { return $Chinese } else { return $English } }

# Stay on screen after an unexpected error instead of closing the window.
trap {
    Write-Host ("`n   " + (T '出错：' 'Error: ') + $_.Exception.Message) -ForegroundColor Red
    Write-Host ('   ' + (T '按任意键退出。' 'Press any key to exit.'))
    try { [Console]::ReadKey($true) | Out-Null } catch {}
    exit 1
}

$AppId = '4115450'
# build.ps1 -Package fills in the PBZ.exe builds this DLL supports: name|size|SHA-256 per build (Steam, WeGame), ';' between.
$ExeBuilds = '__EXE_BUILDS__'
$Here = $PSScriptRoot
$FullDll = Join-Path $Here 'version.dll'
$TriggersDll = Join-Path $Here 'triggers_only\version.dll'

# ART BEGIN
$ArtAvatar = @(
    '       ~[38;2;87;98;109m▄~[0m~[38;2;82;93;104m▄~[0m~[38;2;63;72;80m▄~[0m~[38;2;45;51;57m~[48;2;30;35;38m▀~[0m~[38;2;26;28;30m~[48;2;19;21;21m▀~[0m~[38;2;36;41;46m~[48;2;15;15;15m▀~[0m~[38;2;12;13;14m~[48;2;7;7;7m▀~[0m~[38;2;10;10;9m▄~[0m~[38;2;20;22;24m▄~[0m~[38;2;39;44;50m▄~[0m       '
    '    ~[38;2;9;8;8m▄~[0m~[38;2;66;75;83m~[48;2;10;10;10m▀~[0m~[38;2;86;97;108m~[48;2;23;25;26m▀~[0m~[38;2;81;92;103m~[48;2;24;26;27m▀~[0m~[38;2;68;78;87m~[48;2;33;37;40m▀~[0m~[38;2;47;53;59m~[48;2;36;40;44m▀~[0m~[38;2;38;43;48m~[48;2;37;41;45m▀~[0m~[38;2;29;33;36m~[48;2;42;47;52m▀~[0m~[38;2;30;33;35m~[48;2;44;49;54m▀~[0m~[38;2;23;25;27m~[48;2;28;30;30m▀~[0m~[38;2;12;13;12m~[48;2;12;12;10m▀~[0m~[38;2;18;19;20m~[48;2;22;24;24m▀~[0m~[38;2;37;42;46m~[48;2;37;42;45m▀~[0m~[38;2;65;73;82m~[48;2;48;53;59m▀~[0m~[38;2;68;77;86m~[48;2;32;35;39m▀~[0m~[38;2;14;14;15m▄~[0m    '
    '  ~[38;2;19;19;19m▄~[0m~[38;2;9;9;9m~[48;2;19;19;19m▀~[0m~[38;2;13;13;13m~[48;2;17;17;16m▀~[0m~[38;2;12;12;12m~[48;2;21;21;19m▀~[0m~[38;2;13;13;11m~[48;2;23;23;21m▀~[0m~[38;2;9;9;8m~[48;2;15;16;14m▀~[0m~[38;2;8;8;6m~[48;2;17;17;15m▀~[0m~[38;2;9;9;7m~[48;2;20;19;18m▀~[0m~[38;2;16;16;13m~[48;2;22;20;17m▀~[0m~[38;2;20;19;17m~[48;2;24;22;19m▀~[0m~[38;2;23;21;20m~[48;2;21;17;14m▀~[0m~[38;2;24;21;18m~[48;2;26;22;17m▀~[0m~[38;2;17;15;12m~[48;2;22;18;15m▀~[0m~[38;2;19;19;18m~[48;2;20;18;15m▀~[0m~[38;2;22;23;21m~[48;2;21;21;19m▀~[0m~[38;2;9;9;7m~[48;2;19;19;17m▀~[0m~[38;2;8;8;6m~[48;2;13;13;12m▀~[0m~[38;2;14;15;15m~[48;2;13;13;13m▀~[0m~[38;2;19;22;22m~[48;2;13;14;13m▀~[0m~[38;2;15;18;18m▄~[0m  '
    ' ~[38;2;27;28;28m▄~[0m~[38;2;25;26;26m~[48;2;33;34;32m▀~[0m~[38;2;24;24;23m~[48;2;30;30;28m▀~[0m~[38;2;22;23;20m~[48;2;25;26;24m▀~[0m~[38;2;32;33;30m~[48;2;37;37;35m▀~[0m~[38;2;23;24;22m~[48;2;21;21;19m▀~[0m~[38;2;17;17;15m~[48;2;22;22;20m▀~[0m~[38;2;20;18;16m~[48;2;17;16;14m▀~[0m~[38;2;30;28;25m~[48;2;27;26;24m▀~[0m~[38;2;26;24;20m~[48;2;23;22;19m▀~[0m~[38;2;30;27;24m~[48;2;29;26;23m▀~[0m~[38;2;31;26;22m~[48;2;38;32;26m▀~[0m~[38;2;34;27;22m~[48;2;30;25;21m▀~[0m~[38;2;25;21;18m~[48;2;15;15;14m▀~[0m~[38;2;19;17;15m~[48;2;15;15;14m▀~[0m~[38;2;14;15;13m~[48;2;17;16;14m▀~[0m~[38;2;21;20;18m~[48;2;18;18;16m▀~[0m~[38;2;13;13;11m~[48;2;17;17;15m▀~[0m~[38;2;15;14;14m~[48;2;15;15;14m▀~[0m~[38;2;13;14;13m~[48;2;18;20;20m▀~[0m~[38;2;14;15;15m~[48;2;16;17;18m▀~[0m~[38;2;16;17;18m▄~[0m '
    ' ~[38;2;30;30;29m~[48;2;37;37;36m▀~[0m~[38;2;39;40;38m~[48;2;43;44;42m▀~[0m~[38;2;30;31;28m~[48;2;27;27;25m▀~[0m~[38;2;28;28;26m~[48;2;43;44;41m▀~[0m~[38;2;40;41;38m~[48;2;42;42;39m▀~[0m~[38;2;26;26;23m~[48;2;26;24;22m▀~[0m~[38;2;29;27;25m~[48;2;31;29;28m▀~[0m~[38;2;19;17;14m~[48;2;16;15;13m▀~[0m~[38;2;29;27;24m~[48;2;20;20;17m▀~[0m~[38;2;27;24;21m~[48;2;17;16;14m▀~[0m~[38;2;37;31;26m~[48;2;41;33;27m▀~[0m~[38;2;35;29;24m~[48;2;41;33;27m▀~[0m~[38;2;12;11;10m~[48;2;13;11;11m▀~[0m~[38;2;16;16;14m~[48;2;12;12;12m▀~[0m~[38;2;19;18;15m~[48;2;15;14;13m▀~[0m~[38;2;16;15;13m~[48;2;15;14;13m▀~[0m~[38;2;14;13;12m~[48;2;9;9;8m▀~[0m~[38;2;22;22;20m~[48;2;23;22;21m▀~[0m~[38;2;14;14;14m~[48;2;18;18;17m▀~[0m~[38;2;21;23;23m~[48;2;16;17;17m▀~[0m~[38;2;17;19;21m~[48;2;25;28;30m▀~[0m~[38;2;14;16;15m~[48;2;17;18;20m▀~[0m '
    '~[38;2;31;32;30m~[48;2;41;42;40m▀~[0m~[38;2;48;49;47m~[48;2;52;53;50m▀~[0m~[38;2;48;48;46m~[48;2;38;39;36m▀~[0m~[38;2;28;28;26m~[48;2;37;37;35m▀~[0m~[38;2;54;54;51m~[48;2;50;50;47m▀~[0m~[38;2;32;32;29m~[48;2;23;20;18m▀~[0m~[38;2;24;22;20m~[48;2;24;21;19m▀~[0m~[38;2;31;27;25m~[48;2;21;18;16m▀~[0m~[38;2;18;15;13m~[48;2;21;17;15m▀~[0m~[38;2;19;17;15m~[48;2;30;24;20m▀~[0m~[38;2;25;21;17m~[48;2;57;44;34m▀~[0m~[38;2;45;35;28m~[48;2;73;58;47m▀~[0m~[38;2;65;52;43m~[48;2;77;61;49m▀~[0m~[38;2;37;29;25m~[48;2;60;47;40m▀~[0m~[38;2;13;12;11m~[48;2;22;18;15m▀~[0m~[38;2;18;16;14m~[48;2;24;19;16m▀~[0m~[38;2;18;16;13m~[48;2;28;22;19m▀~[0m~[38;2;13;12;11m~[48;2;24;20;17m▀~[0m~[38;2;15;16;14m~[48;2;13;13;12m▀~[0m~[38;2;20;20;18m~[48;2;16;16;15m▀~[0m~[38;2;18;19;19m~[48;2;22;23;23m▀~[0m~[38;2;19;21;23m~[48;2;21;23;24m▀~[0m~[38;2;20;22;23m~[48;2;18;21;22m▀~[0m~[38;2;19;21;22m~[48;2;20;23;25m▀~[0m'
    '~[38;2;43;44;42m~[48;2;41;41;38m▀~[0m~[38;2;49;49;47m~[48;2;43;43;39m▀~[0m~[38;2;33;33;31m~[48;2;43;43;40m▀~[0m~[38;2;46;46;43m~[48;2;41;40;38m▀~[0m~[38;2;42;40;37m~[48;2;54;46;42m▀~[0m~[38;2;45;38;35m~[48;2;70;64;60m▀~[0m~[38;2;38;30;26m~[48;2;66;53;48m▀~[0m~[38;2;34;26;22m~[48;2;68;50;43m▀~[0m~[38;2;38;30;26m~[48;2;52;39;33m▀~[0m~[38;2;43;32;27m~[48;2;39;28;22m▀~[0m~[38;2;70;54;46m~[48;2;72;55;47m▀~[0m~[38;2;76;60;53m~[48;2;80;61;54m▀~[0m~[38;2;91;73;64m~[48;2;79;63;57m▀~[0m~[38;2;89;72;65m~[48;2;85;68;60m▀~[0m~[38;2;43;33;29m~[48;2;34;27;23m▀~[0m~[38;2;18;15;13m~[48;2;20;17;14m▀~[0m~[38;2;32;24;22m~[48;2;31;23;19m▀~[0m~[38;2;33;25;23m~[48;2;33;25;22m▀~[0m~[38;2;19;16;14m~[48;2;21;18;16m▀~[0m~[38;2;13;13;12m~[48;2;13;12;12m▀~[0m~[38;2;27;28;28m~[48;2;30;32;33m▀~[0m~[38;2;18;20;21m~[48;2;59;67;73m▀~[0m~[38;2;12;13;14m~[48;2;61;69;77m▀~[0m~[38;2;12;13;14m~[48;2;78;88;98m▀~[0m'
    ' ~[38;2;43;43;39m~[48;2;44;44;41m▀~[0m~[38;2;50;50;47m~[48;2;42;42;38m▀~[0m~[38;2;36;34;31m~[48;2;33;31;28m▀~[0m~[38;2;48;39;33m~[48;2;55;44;38m▀~[0m~[38;2;86;76;73m~[48;2;96;80;71m▀~[0m~[38;2;96;80;75m~[48;2;88;71;64m▀~[0m~[38;2;72;54;47m~[48;2;48;36;30m▀~[0m~[38;2;38;27;22m~[48;2;56;46;40m▀~[0m~[38;2;56;43;36m~[48;2;86;72;63m▀~[0m~[38;2;71;55;47m~[48;2;72;59;52m▀~[0m~[38;2;60;48;42m~[48;2;65;53;47m▀~[0m~[38;2;64;52;47m~[48;2;52;40;36m▀~[0m~[38;2;45;36;30m~[48;2;43;33;28m▀~[0m~[38;2;15;13;11m~[48;2;28;22;19m▀~[0m~[38;2;23;18;16m~[48;2;22;18;16m▀~[0m~[38;2;34;26;22m~[48;2;24;20;17m▀~[0m~[38;2;36;27;23m~[48;2;32;25;20m▀~[0m~[38;2;22;21;20m~[48;2;30;29;27m▀~[0m~[38;2;14;13;13m~[48;2;16;16;16m▀~[0m~[38;2;38;42;44m~[48;2;55;57;58m▀~[0m~[38;2;138;153;167m~[48;2;133;146;157m▀~[0m~[38;2;142;159;175m~[48;2;137;153;167m▀~[0m '
    ' ~[38;2;39;38;35m▀~[0m~[38;2;44;42;39m~[48;2;43;41;38m▀~[0m~[38;2;34;30;26m~[48;2;35;29;25m▀~[0m~[38;2;69;57;48m~[48;2;83;68;57m▀~[0m~[38;2;99;82;70m~[48;2;98;82;69m▀~[0m~[38;2;80;65;56m~[48;2;87;71;61m▀~[0m~[38;2;43;34;28m~[48;2;49;39;31m▀~[0m~[38;2;63;54;47m~[48;2;31;26;23m▀~[0m~[38;2;81;71;65m~[48;2;43;36;33m▀~[0m~[38;2;73;61;57m~[48;2;78;69;63m▀~[0m~[38;2;66;53;49m~[48;2;83;76;70m▀~[0m~[38;2;56;46;42m~[48;2;79;74;69m▀~[0m~[38;2;40;33;29m~[48;2;58;53;48m▀~[0m~[38;2;25;21;18m~[48;2;33;29;26m▀~[0m~[38;2;14;13;12m~[48;2;14;13;13m▀~[0m~[38;2;18;16;13m~[48;2;26;23;19m▀~[0m~[38;2;37;29;24m~[48;2;33;28;23m▀~[0m~[38;2;25;23;22m~[48;2;16;15;14m▀~[0m~[38;2;13;14;14m~[48;2;13;13;13m▀~[0m~[38;2;60;64;67m~[48;2;38;43;46m▀~[0m~[38;2;162;172;183m~[48;2;160;172;182m▀~[0m~[38;2;137;152;166m▀~[0m '
    '  ~[38;2;41;37;34m▀~[0m~[38;2;42;35;30m~[48;2;55;45;38m▀~[0m~[38;2;81;67;55m~[48;2;72;58;47m▀~[0m~[38;2;106;89;75m~[48;2;102;85;71m▀~[0m~[38;2;95;79;67m~[48;2;91;75;64m▀~[0m~[38;2;82;67;56m~[48;2;90;74;63m▀~[0m~[38;2;85;72;64m~[48;2;93;80;72m▀~[0m~[38;2;85;72;67m~[48;2;94;81;74m▀~[0m~[38;2;84;71;68m~[48;2;90;77;71m▀~[0m~[38;2;75;60;58m~[48;2;75;61;53m▀~[0m~[38;2;52;38;36m~[48;2;53;42;34m▀~[0m~[38;2;40;28;27m~[48;2;37;30;26m▀~[0m~[38;2;29;22;20m~[48;2;35;29;24m▀~[0m~[38;2;22;20;18m~[48;2;26;22;19m▀~[0m~[38;2;26;23;19m~[48;2;19;17;14m▀~[0m~[38;2;21;19;16m~[48;2;15;14;13m▀~[0m~[38;2;12;12;11m~[48;2;16;17;18m▀~[0m~[38;2;17;18;19m~[48;2;22;24;26m▀~[0m~[38;2;45;51;56m~[48;2;15;17;18m▀~[0m~[38;2;160;173;184m▀~[0m  '
    '    ~[38;2;71;58;46m▀~[0m~[38;2;90;75;62m~[48;2;77;63;52m▀~[0m~[38;2;95;79;67m~[48;2;88;72;59m▀~[0m~[38;2;102;86;74m~[48;2;99;83;70m▀~[0m~[38;2;104;90;80m~[48;2;100;85;72m▀~[0m~[38;2;109;97;90m~[48;2;117;104;95m▀~[0m~[38;2;94;79;71m~[48;2;125;112;106m▀~[0m~[38;2;75;61;53m~[48;2;95;82;74m▀~[0m~[38;2;63;52;44m~[48;2;80;69;62m▀~[0m~[38;2;48;40;35m~[48;2;54;47;41m▀~[0m~[38;2;36;30;26m~[48;2;31;27;24m▀~[0m~[38;2;27;23;19m~[48;2;20;20;19m▀~[0m~[38;2;20;19;18m~[48;2;18;18;18m▀~[0m~[38;2;18;18;19m~[48;2;14;14;14m▀~[0m~[38;2;16;17;19m~[48;2;14;13;13m▀~[0m~[38;2;17;18;19m▀~[0m    '
    '       ~[38;2;70;57;46m▀~[0m~[38;2;78;63;51m▀~[0m~[38;2;85;70;57m▀~[0m~[38;2;101;88;78m~[48;2;53;44;35m▀~[0m~[38;2;96;85;77m~[48;2;52;43;36m▀~[0m~[38;2;80;70;61m~[48;2;38;35;31m▀~[0m~[38;2;44;38;34m~[48;2;24;24;24m▀~[0m~[38;2;24;24;24m▀~[0m~[38;2;14;15;15m▀~[0m~[38;2;11;11;10m▀~[0m       '
)
$ArtTitle = @(
    '                                                                ~[0m'
    '         ~[38;2;247;242;231m██    ▄               █▄               ~[38;2;214;38;38m▄▄▄▄▄▄▄▄▄▄▄▄█▄  ~[0m'
    '   ~[38;2;247;242;231m▀▀▀█▄▄▄▄▄▄▄                 ██  ▄▄▄               ~[38;2;214;38;38m▀█▄   ██   ~[0m'
    '      ~[38;2;247;242;231m█▄▄▄▄▄██            ▀▀▀▀▀▀▀▀███▀            ~[38;2;214;38;38m▄  ▄█    ██   ~[0m'
    '   ~[38;2;247;242;231m█▀ █▄▄▄▄▄▄▄ ▀▀▀              ▄█▀              ~[38;2;214;38;38m▄█  ██    ██   ~[0m'
    '  ~[38;2;247;242;231m▀   ▄▄██  ▄▄▄               ▄▀                 ~[38;2;214;38;38m▀   █▀    ██   ~[0m'
    '     ~[38;2;247;242;231m▀▄▀███▀▄              ▄ ▀                     ~[38;2;214;38;38m▄█▀    ██▀   ~[0m'
    '    ~[38;2;247;242;231m▀▄▄▀▀██  ▀█▄▄▄      ▄▄▀▀▀▄▄▄▄▄▄▄▄▄▄▄          ~[38;2;214;38;38m▄▀    ▄▄██    ~[0m'
    '    ~[38;2;247;242;231m▀   ▄█▀    ▀        ▀     ▀▀▀▀▀▀▀▀▀         ~[38;2;214;38;38m▀        ▀      ~[0m'
    '                                                                ~[0m'
)
$ArtTitleWidth = 64
$ArtAvatarWidth = 24
# ART END

try { [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false) } catch {}
try { $Host.UI.RawUI.WindowTitle = (T '豪之刃 · Cringe Blade Zero 安装' 'Cringe Blade Zero Setup') } catch {}
$E = [char]27
$VT = $false
try { $VT = [bool]$Host.UI.SupportsVirtualTerminal } catch {}
# Block and box-drawing characters can take two cells under the old console's CJK fonts, so the logo and frames are
# drawn only in Windows Terminal / VS Code, and only when the window is wide enough for the logo.
$Fancy = $VT -and ($env:WT_SESSION -or $env:TERM_PROGRAM)
$Wide = $false
try { $Wide = [Console]::WindowWidth -ge (8 + $ArtAvatarWidth + $ArtTitleWidth) } catch {}

function Fg([int]$r, [int]$g, [int]$b) { if ($VT) { "$E[38;2;$r;$g;${b}m" } else { '' } }
function Bg([int]$r, [int]$g, [int]$b) { if ($VT) { "$E[48;2;$r;$g;${b}m" } else { '' } }
$R = if ($VT) { "$E[0m" } else { '' }
$Ivory = Fg 247 242 231; $Gold = Fg 216 194 146; $Red = Fg 214 38 38; $Muted = Fg 140 140 140
$Green = Fg 120 200 130; $Amber = Fg 235 180 80

# Terminal cells: CJK and full-width characters take two.
function Get-Width([string]$Text) {
    $width = 0
    foreach ($ch in $Text.ToCharArray()) {
        $c = [int]$ch
        if (($c -ge 0x1100 -and $c -le 0x115F) -or ($c -ge 0x2E80 -and $c -le 0xA4CF) -or ($c -ge 0xAC00 -and $c -le 0xD7A3) -or
            ($c -ge 0xF900 -and $c -le 0xFAFF) -or ($c -ge 0xFE30 -and $c -le 0xFE4F) -or ($c -ge 0xFF00 -and $c -le 0xFF60) -or
            ($c -ge 0xFFE0 -and $c -le 0xFFE6)) { $width += 2 } else { $width += 1 }
    }
    return $width
}
function Pad([string]$Text, [int]$Width) { return $Text + (' ' * [Math]::Max(0, $Width - (Get-Width $Text))) }
# Long paths keep their end.
function Fit([string]$Text, [int]$Width) {
    if ((Get-Width $Text) -le $Width) { return $Text }
    while ((Get-Width $Text) -gt $Width - 1) { $Text = $Text.Substring(1) }
    return '…' + $Text
}

function Show-Banner {
    Clear-Host
    Write-Host ''
    $tagline = T '《影之刃零》游戏内工具箱 · 一键安装' 'In-game toolkit for Phantom Blade Zero · one-click setup'
    if ($Fancy -and $Wide -and $ArtAvatar) {
        $side = @('', '', "${Gold}C R I N G E   B L A D E   Z E R O$R", "$Muted$tagline$R")
        $right = @($ArtTitle | ForEach-Object { $_.Replace('~', [string]$E) }) + $side
        $rows = [Math]::Max($ArtAvatar.Count, $right.Count)
        $avatarTop = [Math]::Floor(($rows - $ArtAvatar.Count) / 2)
        $rightTop = [Math]::Floor(($rows - $right.Count) / 2)
        for ($i = 0; $i -lt $rows; $i++) {
            $a = $i - $avatarTop; $t = $i - $rightTop
            $line = '   '
            if ($a -ge 0 -and $a -lt $ArtAvatar.Count) { $line += $ArtAvatar[$a].Replace('~', [string]$E) } else { $line += ' ' * $ArtAvatarWidth }
            $line += '     '
            if ($t -ge 0 -and $t -lt $right.Count) { $line += $right[$t] }
            Write-Host $line
        }
    } elseif ($Zh) {
        Write-Host "   ${Ivory}豪 之 ${Red}刃$R   ${Gold}Cringe Blade Zero$R"
        Write-Host "   $Muted$tagline$R"
    } else {
        Write-Host "   ${Ivory}CRINGE BLADE ${Red}ZERO$R"
        Write-Host "   $Muted$tagline$R"
    }
    Write-Host ''
}

# Framed status box; each row is @(label, text, colour).
function Show-Box([object[]]$Rows) {
    $inner = 76
    $label = 14
    if ($Fancy) {
        Write-Host ("   $Muted╭" + ('─' * $inner) + "╮$R")
        foreach ($row in $Rows) {
            $text = Fit $row[1] ($inner - $label - 4)
            Write-Host ("   $Muted│$R  $Gold" + (Pad $row[0] $label) + "$R" + $row[2] + (Pad $text ($inner - $label - 2)) + "$R$Muted│$R")
        }
        Write-Host ("   $Muted╰" + ('─' * $inner) + "╯$R")
    } else {
        Write-Host ('   ' + ('-' * ($inner + 2)))
        foreach ($row in $Rows) { Write-Host ("    $Gold" + (Pad $row[0] $label) + "$R" + $row[2] + (Fit $row[1] ($inner - $label - 4)) + $R) }
        Write-Host ('   ' + ('-' * ($inner + 2)))
    }
    Write-Host ''
}

# ---- Game folder ----------------------------------------------------------------------
# PBZ.exe of a game folder: platform\steam (Steam), another platform\<store> folder, or the folder itself (the PBZ.exe
# folder of another store's install, or typed by hand); a path to PBZ.exe is taken as it is.
function Find-GameExe([string]$Root) {
    if (-not $Root) { return $null }
    $Root = $Root.Trim().Trim('"').TrimEnd('\')
    if (-not $Root) { return $null }
    if ($Root -match '\\PBZ\.exe$') { if (Test-Path -LiteralPath $Root -PathType Leaf) { return $Root } else { return $null } }
    $candidates = @((Join-Path $Root 'platform\steam\PBZ.exe'))
    $platform = Join-Path $Root 'platform'
    if (Test-Path -LiteralPath $platform -PathType Container) {
        $candidates += @(Get-ChildItem -LiteralPath $platform -Directory -ErrorAction SilentlyContinue | Sort-Object Name | ForEach-Object { Join-Path $_.FullName 'PBZ.exe' })
    }
    $candidates += @((Join-Path $Root 'PBZ.exe'))
    foreach ($candidate in $candidates) { if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate } }
    return $null
}
function Read-RegistryValue([string]$Key, [string]$Name) {
    try { return (Get-ItemProperty -LiteralPath $Key -Name $Name -ErrorAction Stop).$Name } catch { return $null }
}
# Every install found: @(PBZ.exe, where it was found), one per PBZ.exe.
function Find-Games {
    $games = @()
    foreach ($key in @("HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Steam App $AppId",
                       "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Steam App $AppId",
                       "HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Steam App $AppId")) {
        $exe = Find-GameExe (Read-RegistryValue $key 'InstallLocation')
        if ($exe) { $games += ,@($exe, (T 'Steam 卸载项' 'Steam uninstall entry')) }
    }
    $steams = @((Read-RegistryValue 'HKCU:\Software\Valve\Steam' 'SteamPath'),
                (Read-RegistryValue 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam' 'InstallPath'),
                (Read-RegistryValue 'HKLM:\SOFTWARE\Valve\Steam' 'InstallPath')) | Where-Object { $_ }
    foreach ($steam in $steams) {
        $libraries = @($steam.Replace('/', '\'))
        $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
        if (Test-Path -LiteralPath $vdf) {
            foreach ($match in [regex]::Matches((Get-Content -LiteralPath $vdf -Raw -Encoding UTF8), '"path"\s+"([^"]+)"')) {
                $libraries += $match.Groups[1].Value.Replace('\\', '\')
            }
        }
        foreach ($library in ($libraries | Select-Object -Unique)) {
            $manifest = Join-Path $library "steamapps\appmanifest_$AppId.acf"
            if (-not (Test-Path -LiteralPath $manifest)) { continue }
            $dir = [regex]::Match((Get-Content -LiteralPath $manifest -Raw -Encoding UTF8), '"installdir"\s+"([^"]+)"').Groups[1].Value
            $exe = Find-GameExe (Join-Path $library "steamapps\common\$dir")
            if ($exe) { $games += ,@($exe, (T 'Steam 库' 'Steam library')) }
        }
    }
    # Other stores (WeGame): an uninstall entry named after the game, if the store writes one.
    foreach ($hive in @('HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall', 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall',
                        'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall')) {
        foreach ($entry in @(Get-ChildItem -LiteralPath $hive -ErrorAction SilentlyContinue)) {
            $name = Read-RegistryValue $entry.PSPath 'DisplayName'
            if (-not $name -or $name -notmatch '影之刃零|Phantom Blade Zero') { continue }
            $exe = Find-GameExe (Read-RegistryValue $entry.PSPath 'InstallLocation')
            if ($exe) { $games += ,@($exe, ((T '卸载项 ' 'uninstall entry ') + $name)) }
        }
    }
    $seen = @{}; $unique = @()
    foreach ($game in $games) {
        $key = $game[0].ToLowerInvariant()
        if (-not $seen.ContainsKey($key)) { $seen[$key] = $true; $unique += ,$game }
    }
    return ,$unique
}
function Read-GameExe {
    while ($true) {
        Write-Host ("   $Amber" + (T '没有在注册表中找到《影之刃零》。' 'Phantom Blade Zero was not found in the registry.') + $R)
        $path = Read-Host ('   ' + (T '请输入游戏目录（含 platform 文件夹的那一层或 PBZ.exe 所在文件夹，也可粘贴 PBZ.exe 路径；直接回车退出）' 'Game folder (the one that contains "platform", or the folder of PBZ.exe; a path to PBZ.exe also works; Enter to quit)'))
        if (-not $path) { exit 1 }
        $path = $path.Trim().Trim('"')
        $candidates = @($path)
        try {
            $parent = Split-Path $path -Parent
            if ($parent) { $candidates += $parent; $grand = Split-Path $parent -Parent; if ($grand) { $candidates += $grand } }
        } catch {}
        foreach ($candidate in $candidates) {
            $exe = Find-GameExe $candidate
            if ($exe) { return @($exe, (T '手动输入' 'entered by hand')) }
        }
        Write-Host ("   $Red" + (T '这个目录下没有 PBZ.exe（Steam 版在 platform\steam 里）。' 'There is no PBZ.exe under this folder (the Steam version keeps it in platform\steam).') + $R)
    }
}
function Select-Game([object[]]$Games) {
    if ($Games.Count -eq 1) { return $Games[0] }
    Write-Host ('   ' + (T '找到多个《影之刃零》安装，选择要安装到哪一个：' 'Found more than one Phantom Blade Zero install; pick one:'))
    Write-Host ''
    $items = @()
    for ($i = 0; $i -lt [Math]::Min($Games.Count, 9); $i++) { $items += ,@([string]($i + 1), (Fit (Split-Path $Games[$i][0]) 52), $Games[$i][1]) }
    $items += ,@('0', (T '退出' 'Quit'), '')
    $selected = Read-Choice $items
    if ($selected -eq '0') { exit 0 }
    return $Games[[int]$selected - 1]
}

# ---- State ----------------------------------------------------------------------------
function Get-Sha([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-', '').ToLowerInvariant() }
    finally { $algorithm.Dispose(); $stream.Dispose() }
}
# The full DLL carries PBZDebugMenu strings, the triggers-only DLL a wide PBZTriggersOff.log; Latin-1 maps bytes 1:1,
# so both forms can be searched in one string.
function Test-OurDll([string]$Path) {
    $text = [Text.Encoding]::GetEncoding(28591).GetString([IO.File]::ReadAllBytes($Path))
    foreach ($mark in @('PBZDebugMenu', 'PBZTriggersOff')) {
        if ($text.Contains($mark) -or $text.Contains((($mark.ToCharArray()) -join "`0") + "`0")) { return $true }
    }
    return $false
}
function Get-InstallState([string]$Target) {
    if (-not (Test-Path -LiteralPath $Target)) { return @((T '未安装' 'Not installed'), $Muted, 'none') }
    $sha = Get-Sha $Target
    if ((Test-Path -LiteralPath $FullDll) -and $sha -eq (Get-Sha $FullDll)) { return @((T '已安装完整版（本包版本）' 'Full version (this package)'), $Green, 'full') }
    if ((Test-Path -LiteralPath $TriggersDll) -and $sha -eq (Get-Sha $TriggersDll)) { return @((T '已安装精简版（本包版本）' 'Triggers-only (this package)'), $Green, 'triggers') }
    if (Test-OurDll $Target) { return @((T '已安装豪之刃的其他版本' 'Another Cringe Blade Zero build'), $Amber, 'ours') }
    return @((T '有其他 MOD 的 version.dll（安装时会备份）' "Another mod's version.dll (backed up on install)"), $Amber, 'foreign')
}
function Get-GameVersion([string]$Exe) {
    if ($ExeBuilds.StartsWith('__')) { return @((T '未校验（开发版安装脚本）' 'Not checked (development installer)'), $Muted, $true) }
    $size = (Get-Item -LiteralPath $Exe).Length
    $sha = $null
    foreach ($build in $ExeBuilds.Split(';')) {
        $fields = $build.Split('|')
        if ($fields.Count -ne 3 -or $size -ne [long]$fields[1]) { continue }
        if (-not $sha) { $sha = Get-Sha $Exe }
        if ($sha -eq $fields[2]) { return @(((T '与本 MOD 匹配（{0} 版）' 'Matches this build ({0})') -f $fields[0]), $Green, $true) }
    }
    return @((T '与本 MOD 不匹配：只有关闭自适应扳机可用' 'Does not match: only the adaptive-trigger switch works'), $Red, $false)
}

# ---- Menu -----------------------------------------------------------------------------
function Read-Choice([object[]]$Items) {
    $labelWidth = 2 + ($Items | ForEach-Object { Get-Width $_[1] } | Measure-Object -Maximum).Maximum
    $noteWidth = ($Items | ForEach-Object { Get-Width $_[2] } | Measure-Object -Maximum).Maximum
    $index = 0
    $top = [Console]::CursorTop
    while ($true) {
        [Console]::SetCursorPosition(0, $top)
        for ($i = 0; $i -lt $Items.Count; $i++) {
            $item = $Items[$i]
            $label = Pad $item[1] $labelWidth
            if ($i -eq $index) {
                if ($Fancy) { Write-Host ('   ' + (Bg 72 22 22) + "$Gold ▶ " + $item[0] + "  $Ivory" + $label + $Gold + (Pad $item[2] $noteWidth) + " $R") }
                else { Write-Host ("   > " + $item[0] + '  ' + $label + (Pad $item[2] $noteWidth) + ' ') }
            } else {
                Write-Host ("     $Muted" + $item[0] + "$R  " + $label + $Muted + (Pad $item[2] $noteWidth) + " $R")
            }
        }
        Write-Host ''
        Write-Host ("   $Muted" + (T '↑ ↓ 选择 · Enter 确认 · 数字键直接选 · Esc 退出' '↑ ↓ select · Enter confirm · number keys pick · Esc quit') + $R) -NoNewline
        # A short window scrolls on the first draw; find the menu's first row again from where the cursor ended.
        $top = [Console]::CursorTop - $Items.Count - 1
        $key = [Console]::ReadKey($true)
        switch ($key.Key) {
            'UpArrow' { $index = ($index - 1 + $Items.Count) % $Items.Count; continue }
            'DownArrow' { $index = ($index + 1) % $Items.Count; continue }
            'Enter' { return $Items[$index][0] }
            'Escape' { return '0' }
        }
        foreach ($item in $Items) { if ($item[0] -eq [string]$key.KeyChar) { return $item[0] } }
    }
}

function Wait-GameClosed {
    while (Get-Process -Name 'PBZ' -ErrorAction SilentlyContinue) {
        Write-Host ("   $Amber" + (T '游戏正在运行，请先退出游戏，然后按 Enter 继续（Esc 返回）。' 'The game is running. Quit it, then press Enter to continue (Esc to go back).') + $R)
        if ([Console]::ReadKey($true).Key -eq 'Escape') { return $false }
    }
    return $true
}
function Test-AccessDenied($ErrorRecord) {
    for ($e = $ErrorRecord.Exception; $e; $e = $e.InnerException) {
        if ($e -is [UnauthorizedAccessException] -or $e.HResult -eq -2147024891) { return $true }
    }
    return $false
}
function Restart-Elevated([string]$Selected) {
    Write-Host ("   $Amber" + (T '没有写入权限。按 A 以管理员身份重试，其他键返回。' 'No write access. Press A to retry as administrator, any other key to go back.') + $R)
    if ([Console]::ReadKey($true).Key -ne 'A') { return }
    Start-Process powershell.exe -Verb RunAs -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-Choice', $Selected)
    exit 0
}

function Install-SampleCombo([string]$GameFolder) {
    $source = Join-Path $Here 'samples\sampleCombo.combo'
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw (T '安装包缺少 sampleCombo.combo。' 'sampleCombo.combo is missing from the package.') }
    $sourceSha = Get-Sha $source
    $folder = Join-Path $GameFolder 'combos'
    New-Item -ItemType Directory -Path $folder -Force | Out-Null
    $target = Join-Path $folder 'sampleCombo.combo'
    if ((Test-Path -LiteralPath $target) -and (Get-Sha $target) -ne $sourceSha) {
        # Preserve the player's edited example. Never touch default or the active-file setting.
        $stem = 'sampleCombo_bundled_' + $sourceSha.Substring(0, 8)
        $target = Join-Path $folder ($stem + '.combo')
        $suffix = 1
        while ((Test-Path -LiteralPath $target) -and (Get-Sha $target) -ne $sourceSha) {
            $target = Join-Path $folder ($stem + '_' + $suffix + '.combo'); $suffix++
        }
    }
    if (-not (Test-Path -LiteralPath $target)) { Copy-Item -LiteralPath $source -Destination $target }
    if ((Get-Sha $target) -ne $sourceSha) { throw (T '示例连招校验失败。' 'Sample combo verification failed.') }
    Write-Host ('   ' + (T '示例已就绪：' 'Sample ready: ') + $target)
    Write-Host ('   ' + (T '在“招式派生”的文件列表选择该示例即可使用；未修改 default 或当前选中的文件。' 'Select this example in the combo editor file list; default and the active file were left unchanged.'))
    return $target
}

function Install-Dll([string]$Selected, [string]$Target) {
    $source = if ($Selected -eq '1') { $FullDll } else { $TriggersDll }
    $name = if ($Selected -eq '1') { T '完整版' 'the full version' } else { T '精简版' 'triggers-only' }
    if (-not (Test-Path -LiteralPath $source)) { Write-Host ("   $Red" + (T '安装包里缺少 ' 'Missing from the package: ') + $source + $R); return $false }
    if ($Selected -eq '1' -and -not (Test-Path -LiteralPath (Join-Path $Here 'samples\sampleCombo.combo') -PathType Leaf)) {
        Write-Host (T '安装包缺少 sampleCombo.combo，请重新解压完整安装包。' 'sampleCombo.combo is missing; extract the complete package again.'); return $false
    }
    if (-not (Wait-GameClosed)) { return $false }
    try {
        if ((Test-Path -LiteralPath $Target) -and -not (Test-OurDll $Target)) {
            $backup = $Target + '.backup_' + (Get-Date -Format 'yyyyMMdd_HHmmss')
            Copy-Item -LiteralPath $Target -Destination $backup -Force
            Write-Host ("   $Amber" + (T '原有的 version.dll 不属于豪之刃，已备份为 ' 'The existing version.dll belongs to another mod; backed up as ') + (Split-Path $backup -Leaf) + $R)
        }
        Copy-Item -LiteralPath $source -Destination $Target -Force
        # The copy keeps the ZIP's "downloaded from the internet" mark, which some security software refuses.
        Unblock-File -LiteralPath $Target -ErrorAction SilentlyContinue
        if ($Selected -eq '1') { [void](Install-SampleCombo ([IO.Path]::GetDirectoryName($Target))) }
    } catch {
        if (Test-AccessDenied $_) { Restart-Elevated $Selected; return $false }
        Write-Host ("   $Red" + (T '复制失败：' 'Copy failed: ') + $_.Exception.Message + $R); return $false
    }
    if ((Get-Sha $Target) -ne (Get-Sha $source)) { Write-Host ("   $Red" + (T '校验失败：写入的文件与安装包不一致。' 'Verification failed: the written file differs from the package.') + $R); return $false }
    Write-Host ("   $Green" + (T "✓ 已安装$name" "✓ Installed $name") + "$R  $Muted$Target$R")
    Write-Host "   ${Muted}SHA-256 $(Get-Sha $Target)$R"
    if ($Selected -eq '1') {
        # Single quotes: PowerShell also ends double-quoted strings at the Chinese quotation marks.
        $hint = T '启动游戏，屏幕上方出现“豪之刃 已加载”横幅后按 {0} 打开面板。' 'Start the game; once the "Cringe Blade Zero loaded" banner shows at the top, press {0} for the panel.'
        Write-Host ('   ' + ($hint -f "${Gold}F10$R"))
    } else {
        Write-Host ('   ' + (T '启动游戏即可，DualSense 自适应扳机已关闭。' 'Start the game; DualSense adaptive triggers are now off.'))
    }
    return $true
}
function Uninstall-Dll([string]$Target) {
    if (-not (Test-Path -LiteralPath $Target)) { Write-Host ("   $Muted" + (T '没有安装，不需要卸载。' 'Nothing is installed.') + $R); return }
    if (-not (Test-OurDll $Target)) { Write-Host ("   $Amber" + (T 'PBZ.exe 旁边的 version.dll 不属于豪之刃，没有删除。' 'The version.dll next to PBZ.exe belongs to another mod; left in place.') + $R); return }
    if (-not (Wait-GameClosed)) { return }
    try { Remove-Item -LiteralPath $Target -Force } catch {
        if (Test-AccessDenied $_) { Restart-Elevated '3'; return }
        Write-Host ("   $Red" + (T '删除失败：' 'Delete failed: ') + $_.Exception.Message + $R); return
    }
    $backup = Get-ChildItem -LiteralPath (Split-Path $Target) -Filter 'version.dll.backup_*' -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1
    if ($backup) {
        Move-Item -LiteralPath $backup.FullName -Destination $Target
        Write-Host ("   $Green" + (T '✓ 已卸载，并还原了安装前备份的 version.dll' '✓ Uninstalled; the version.dll backed up at install time is restored') + $R)
    } else { Write-Host ("   $Green" + (T '✓ 已卸载' '✓ Uninstalled') + $R) }
    Write-Host ("   $Muted" + (T '设置文件 PBZDebugMenu.ini 与日志保留在 PBZ.exe 所在文件夹，可手动删除。' 'PBZDebugMenu.ini and the logs stay next to PBZ.exe; delete them by hand if you want.') + $R)
}

# EnvCheck.exe (same folder): checks runtimes, system parts, the game and the install, and repairs what it can; it
# runs in this window and asks for administrator rights itself when a repair needs them.
function Invoke-EnvCheck([string]$Exe) {
    $tool = Join-Path $Here 'EnvCheck.exe'
    if (-not (Test-Path -LiteralPath $tool)) { Write-Host ("   $Red" + (T '安装包里缺少 EnvCheck.exe' 'EnvCheck.exe is missing from the package') + $R); return }
    Clear-Host
    & $tool --game $Exe --package $Here --lang $(if ($Zh) { 'zh' } else { 'en' }) --no-pause
}

# ---- Main -----------------------------------------------------------------------------
Show-Banner
$games = Find-Games
$found = if ($games.Count) { Select-Game $games } else { Read-GameExe }
$exe = $found[0]
$folder = Split-Path $exe
$target = Join-Path $folder 'version.dll'
Write-Host ("   $Muted" + (T '正在校验游戏版本…' 'Checking the game version…') + $R)
$version = Get-GameVersion $exe
while ($true) {
    $state = Get-InstallState $target
    Show-Banner
    Show-Box @(@((T '游戏目录' 'Game folder'), "$folder ($($found[1]))", $Ivory), @((T '游戏版本' 'Game version'), $version[0], $version[1]),
               @((T '当前状态' 'Installed'), $state[0], $state[1]))
    $selected = $Choice
    $Choice = ''
    if (-not $selected) {
        $selected = Read-Choice @(
            @('1', (T '安装完整版' 'Install full'), (T '面板（F10）与全部功能，含关闭自适应扳机' 'Panel (F10) and every feature, triggers off included')),
            @('2', (T '安装精简版' 'Install triggers-only'), (T '只关闭 DualSense 自适应扳机' 'Only turns off DualSense adaptive triggers')),
            @('3', (T '卸载' 'Uninstall'), (T '删除 version.dll，保留设置文件' 'Removes version.dll, keeps the settings file')),
            @('4', (T '环境检查与修复' 'Check and repair'), (T '运行库、系统组件、游戏版本、安装位置，发现问题可一键修复' 'Runtimes, system parts, game build, install location; repairs what it finds')),
            @('5', (T '安装示例连招 sampleCombo' 'Install sampleCombo'), (T '保留 default 和已有的自定义示例' 'Keeps default and edited examples')),
            @('0', (T '退出' 'Quit'), ''))
    }
    Write-Host ''
    switch ($selected) {
        '1' { [void](Install-Dll '1' $target) }
        '2' { [void](Install-Dll '2' $target) }
        '3' { Uninstall-Dll $target }
        '4' { Invoke-EnvCheck $exe }
        '5' { if (Wait-GameClosed) { try { [void](Install-SampleCombo $folder) } catch { if (Test-AccessDenied $_) { Restart-Elevated '5' } else { Write-Host $_.Exception.Message -ForegroundColor Red } } } }
        default { exit 0 }
    }
    Write-Host ''
    Write-Host ("   $Muted" + (T '按任意键返回，Esc 退出。' 'Press any key to go back, Esc to quit.') + $R)
    if ([Console]::ReadKey($true).Key -eq 'Escape') { exit 0 }
}
