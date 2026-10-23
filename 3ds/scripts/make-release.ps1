# Builds a release of OpenMW for New 3DS into out\release\<version>\ (docs/RELEASE.md):
#   OpenMW-3DS-<version>-Windows.zip     the setup app and the port's files (OpenMW-3DS-Setup-<version>\)
#   OpenMW-3DS-<version>-Python.zip      the Python backup of the setup, with the port's files and its README
#   openmw3ds-<version>.cia              the CIA alone, for updating an installed copy
#   port-<version>.zip                   the port's files alone, for the macOS and Linux builds
#   (no checksum file: GitHub shows a SHA-256 for each download)
# The macOS and Linux setup apps are built on GitHub (.github/workflows/setup-app.yml), with the port's files from
# the release: -Draft makes the draft release with these files, then starts that build, which adds
# OpenMW-3DS-<version>-macOS.zip and OpenMW-3DS-<version>-Linux.tar.gz to it.
#
#   scripts\make-release.ps1 -Version 0.1.0 [-SkipBuild] [-Draft]
#
# The port's files are the engine's and the port's own only: no game data (the setup app makes that from the player's
# Morrowind). The CIA is a release build (no debug tools) in build\openmw-release, the test builds' build\openmw is
# left alone.
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [switch]$SkipBuild,
    [switch]$Draft,
    [string]$Repository = 'Synaelle/OpenMW-3DS'
)
$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "version must be major.minor.micro: $Version" }
$Build = Join-Path $RepoRoot 'build\openmw-release'
$Out = Join-Path $RepoRoot "out\release\$Version"
$Setup = Join-Path $RepoRoot 'setup'
$SetupExe = Join-Path $Setup 'target\release\openmw3ds-setup.exe'

# The setup app's version is its workspace's (setup\Cargo.toml): the same as the release's.
$setupVersion = (Select-String -Path (Join-Path $Setup 'Cargo.toml') -Pattern '^version = "(.+)"').Matches[0].Groups[1].Value
if ($setupVersion -ne $Version) { throw "setup\Cargo.toml says $setupVersion, the release is $Version" }

# 1. The game: a release build, its version in the CIA and on screen.
if (-not $SkipBuild) {
    & "$PSScriptRoot\build-openmw.ps1" -NoDebug -Version $Version -BuildDir 'build\openmw-release' | Out-Host
}
$cia = Join-Path $Build 'apps\openmw\openmw.cia'
if (-not (Test-Path $cia)) { throw "no release CIA in $Build" }

# 2. The setup app for Windows (Rust and Tauri, the MSVC toolchain: one executable, WebView2 comes with Windows).
if (-not $SkipBuild) {
    Push-Location (Join-Path $Setup 'app')
    try {
        $env:RUSTUP_TOOLCHAIN = 'stable-x86_64-pc-windows-msvc'
        & npx --yes '@tauri-apps/cli@2.12.4' build --no-bundle | Out-Host
        if ($LASTEXITCODE -ne 0) { throw 'setup app: build failed' }
    } finally {
        Remove-Item Env:RUSTUP_TOOLCHAIN -ErrorAction SilentlyContinue
        Pop-Location
    }
}
if (-not (Test-Path $SetupExe)) { throw "no setup app in $SetupExe" }

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

# A README from setup\packaging, with the version.
function New-Readme([string]$Path, [string]$Platform) {
    (Get-Content -Raw -Encoding utf8 (Join-Path $Setup "packaging\README-$Platform.txt")).Replace('{VERSION}', $Version) |
        Set-Content -Encoding utf8 -NoNewline $Path
}

# A zip with '/' in its names (Windows PowerShell's Compress-Archive writes '\', which other systems take as part of
# the name): Windows' own tar.
function New-Zip([string]$Folder, [string[]]$Items, [string]$Zip) {
    & "$env:SystemRoot\System32\tar.exe" -a -c -f $Zip -C $Folder @Items
    if ($LASTEXITCODE -ne 0) { throw "zipping $Zip failed" }
}

$folder = "OpenMW-3DS-Setup-$Version"

# Windows: the app alone (no command line: the tester, it confuses players).
$stage = Join-Path $Out "stage-windows\$folder"
New-PortFolder (Join-Path $stage 'port')
Copy-Item $SetupExe $stage
New-Readme (Join-Path $stage 'README.txt') 'Windows'
New-Zip (Join-Path $Out 'stage-windows') @($folder) (Join-Path $Out "OpenMW-3DS-$Version-Windows.zip")

# The port's files alone: the macOS and Linux apps carry them inside.
$stage = Join-Path $Out 'stage-port'
New-PortFolder $stage
New-Zip $stage @('cias', 'openmw') (Join-Path $Out "port-$Version.zip")

# The Python backup, its own download (the tester): the script, its lists, the port's files.
$stage = Join-Path $Out "stage-python\OpenMW-3DS-Setup-Python-$Version"
New-PortFolder (Join-Path $stage 'port')
New-Item -ItemType Directory -Force (Join-Path $stage 'data') | Out-Null
Copy-Item (Join-Path $Setup 'python\openmw3ds_setup.py') $stage
Copy-Item (Join-Path $Setup 'data\*.txt') (Join-Path $stage 'data')
New-Readme (Join-Path $stage 'README.txt') 'Python'
New-Zip (Join-Path $Out 'stage-python') @("OpenMW-3DS-Setup-Python-$Version") (Join-Path $Out "OpenMW-3DS-$Version-Python.zip")

Copy-Item $cia (Join-Path $Out "openmw3ds-$Version.cia")
foreach ($s in 'stage-windows', 'stage-port', 'stage-python') {
    if (Test-Path (Join-Path $Out $s)) { Remove-Item -Recurse -Force (Join-Path $Out $s) }
}

Get-ChildItem $Out | Select-Object Name, @{ n = 'MB'; e = { [math]::Round($_.Length / 1MB, 1) } } | Format-Table -AutoSize

# The draft release with these files, then the macOS and Linux builds on GitHub, which add theirs to it. Publishing
# it stays a person's call (its notes first).
if ($Draft) {
    $files = Get-ChildItem $Out | ForEach-Object FullName
    & gh release create $Version --repo $Repository --draft --title "OpenMW for New 3DS $Version" --notes '' @files
    if ($LASTEXITCODE -ne 0) { throw 'the draft release failed' }
    & gh workflow run setup-app.yml --repo $Repository -f "tag=$Version"
    if ($LASTEXITCODE -ne 0) { throw 'starting the macOS and Linux builds failed' }
    Write-Output "Draft release $Version made. The macOS and Linux builds are running: gh run list --repo $Repository --workflow setup-app.yml"
}
