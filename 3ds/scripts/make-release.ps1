# Builds a release of OpenMW for New 3DS into out\release\<version>\ (docs/RELEASE.md):
#   OpenMW-3DS-<version>-Windows.zip     the setup app and the port's files (OpenMW-3DS-Setup-<version>\)
#   OpenMW-3DS-<version>-Linux.tar.gz    the same with the Linux AppImage (built in WSL with Docker)
#   OpenMW-3DS-<version>-Python.zip      the Python backup of the setup, with the port's files and its README
#   openmw3ds-<version>.cia              the CIA alone, for updating an installed copy
#   (no checksum file: GitHub shows a SHA-256 for each download)
#
#   scripts\make-release.ps1 -Version 0.1.0 [-SkipBuild] [-SkipLinux]
#
# The port's files are the engine's and the port's own only: no game data (the setup app makes that from the player's
# Morrowind). The CIA is a release build (no debug tools) in build\openmw-release, the test builds' build\openmw is
# left alone.
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [switch]$SkipBuild,
    [switch]$SkipLinux,
    [string]$Msys2Root = 'C:\msys64'
)
$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "version must be major.minor.micro: $Version" }
$Build = Join-Path $RepoRoot 'build\openmw-release'
$Out = Join-Path $RepoRoot "out\release\$Version"
$Setup = Join-Path $RepoRoot 'setup'

# 1. The game: a release build, its version in the CIA and on screen.
if (-not $SkipBuild) {
    & "$PSScriptRoot\build-openmw.ps1" -NoDebug -Version $Version -BuildDir 'build\openmw-release' | Out-Host
}
$cia = Join-Path $Build 'apps\openmw\openmw.cia'
if (-not (Test-Path $cia)) { throw "no release CIA in $Build" }

# 2. The setup app for Windows (static executables).
if (-not $SkipBuild) {
    $env:PATH = "$Msys2Root\ucrt64\bin;$env:PATH"
    & cmake -S $Setup -B (Join-Path $Setup 'build-release') -G Ninja -DCMAKE_BUILD_TYPE=Release | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'setup app: configure failed' }
    & cmake --build (Join-Path $Setup 'build-release') | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'setup app: build failed' }
}
# 3. The setup app for Linux: the AppImage, in WSL (Docker, Ubuntu 22.04).
if (-not $SkipLinux -and -not $SkipBuild) {
    $wslSetup = $Setup -replace '\\', '/' -replace '^([A-Za-z]):', { "/mnt/$($_.Groups[1].Value.ToLower())" }
    & wsl.exe -d Debian -- bash -lc ("cd '$wslSetup' && " + 'sed -i "s/\r$//" linux/build-appimage.sh && bash linux/build-appimage.sh') | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'setup app: the AppImage build failed' }
}

# Emptied rather than removed: an Explorer window open on it keeps the folder itself locked.
New-Item -ItemType Directory -Force $Out | Out-Null
Get-ChildItem $Out | Remove-Item -Recurse -Force

# The port's files, laid out as on the SD card.
function New-PortFolder([string]$Dst) {
    $openmw = Join-Path $Dst 'openmw'
    New-Item -ItemType Directory -Force (Join-Path $openmw 'config'), (Join-Path $Dst 'cias') | Out-Null
    Copy-Item (Join-Path $Build 'openmw.cfg') $openmw
    Copy-Item (Join-Path $Build 'defaults.bin') $openmw
    Copy-Item -Recurse (Join-Path $Build 'resources') (Join-Path $openmw 'resources')
    Copy-Item -Recurse (Join-Path $RepoRoot 'packaging\ui3ds') (Join-Path $openmw 'ui3ds')
    Copy-Item (Join-Path $RepoRoot 'packaging\settings-3ds.cfg') (Join-Path $openmw 'config\settings.cfg')
    Copy-Item $cia (Join-Path $Dst 'cias\openmw3ds.cia')
}

function New-Readme([string]$Path, [string]$Program) {
    @"
OpenMW for New 3DS $Version
For the New 3DS and New 2DS.

1. Run $Program. It finds your Morrowind (English, from Steam, GOG or the disc) or lets you pick its folder.
2. Press Prepare. Your game's files are added to the "port" folder next to the program, or to a folder you choose.
3. Copy that folder's contents to the root of your SD card. Over Wi-Fi this is slow for now, a faster way is coming.
4. Install cias/openmw3ds.cia with FBI (https://github.com/Steveice10/FBI/releases/latest) or Cement
   (https://github.com/Synaelle/cement/releases/latest), then start OpenMW. The first launch takes a while.

Keep the "port" folder next to the program.
It's a CIA and not a .3dsx because the game needs all the memory the New 3DS gives an installed title.
Your game's files are only read, never changed, and none of them are in this download.
"@ | Set-Content -Encoding utf8 $Path
}

function New-PythonReadme([string]$Path) {
    @"
OpenMW for New 3DS $Version, the setup in Python
A backup for when the setup app doesn't run on your computer. It does the same thing without a window.

1. Install Python 3 (https://www.python.org), then in a terminal: pip install pillow numpy
2. In this folder, run:
   python openmw3ds_setup.py --morrowind "the folder Morrowind is installed in"
   Your game's files are added to the "port" folder here. To put them somewhere else, add --output "a folder".
3. Copy that folder's contents to the root of your SD card. Over Wi-Fi this is slow for now, a faster way is coming.
4. Install cias/openmw3ds.cia with FBI (https://github.com/Steveice10/FBI/releases/latest) or Cement
   (https://github.com/Synaelle/cement/releases/latest), then start OpenMW. The first launch takes a while.

It takes a minute or two. The textures come out very slightly different from the app's, which doesn't change how the
game looks in practice. Keep the "port" and "data" folders next to the script. Only the English Morrowind works for
now. Your game's files are only read, never changed, and none of them are in this download.
"@ | Set-Content -Encoding utf8 $Path
}

$folder = "OpenMW-3DS-Setup-$Version"

# Windows: the app alone (no command line: the tester, it confuses players).
$stage = Join-Path $Out "stage-windows\$folder"
New-PortFolder (Join-Path $stage 'port')
Copy-Item (Join-Path $Setup 'build-release\openmw3ds-setup.exe') $stage
New-Readme (Join-Path $stage 'README.txt') 'openmw3ds-setup.exe'
Compress-Archive -Path $stage -DestinationPath (Join-Path $Out "OpenMW-3DS-$Version-Windows.zip")

# Linux (a tar keeps the AppImage executable).
if (-not $SkipLinux) {
    $stage = Join-Path $Out "stage-linux\$folder"
    New-PortFolder (Join-Path $stage 'port')
    Copy-Item (Join-Path $Setup 'out\OpenMW-3DS-Setup-x86_64.AppImage') $stage
    New-Readme (Join-Path $stage 'README.txt') 'OpenMW-3DS-Setup-x86_64.AppImage'
    $linuxStage = (Join-Path $Out 'stage-linux') -replace '\\', '/' -replace '^([A-Za-z]):', { "/mnt/$($_.Groups[1].Value.ToLower())" }
    & wsl.exe -d Debian -- bash -lc "cd '$linuxStage' && chmod +x $folder/OpenMW-3DS-Setup-x86_64.AppImage && tar -czf ../OpenMW-3DS-$Version-Linux.tar.gz $folder"
    if ($LASTEXITCODE -ne 0) { throw 'the Linux archive failed' }
}

# The Python backup, its own download (the tester): the script, its lists, the port's files.
$stage = Join-Path $Out "stage-python\OpenMW-3DS-Setup-Python-$Version"
New-PortFolder (Join-Path $stage 'port')
New-Item -ItemType Directory -Force (Join-Path $stage 'data') | Out-Null
Copy-Item (Join-Path $Setup 'python\openmw3ds_setup.py') $stage
Copy-Item (Join-Path $Setup 'data\*.txt') (Join-Path $stage 'data')
New-PythonReadme (Join-Path $stage 'README.txt')
Compress-Archive -Path $stage -DestinationPath (Join-Path $Out "OpenMW-3DS-$Version-Python.zip")

Copy-Item $cia (Join-Path $Out "openmw3ds-$Version.cia")
foreach ($s in 'stage-windows', 'stage-linux', 'stage-python') {
    if (Test-Path (Join-Path $Out $s)) { Remove-Item -Recurse -Force (Join-Path $Out $s) }
}

Get-ChildItem $Out | Select-Object Name, @{ n = 'MB'; e = { [math]::Round($_.Length / 1MB, 1) } } | Format-Table -AutoSize
