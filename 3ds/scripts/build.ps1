# One-command build: configures + builds the 3DS superbuild in MSYS2 and copies artefacts to out\.
#   scripts\build.ps1                       # RelWithDebInfo, all targets
#   scripts\build.ps1 -Config Debug -Clean
# Outputs: out\<target>.3dsx, out\<target>.cia, out\<target>.elf (keep the .elf for symbolize.ps1)
param(
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string]$Config = 'RelWithDebInfo',
    [string]$Target = 'all',
    [switch]$Clean,
    [string]$Msys2Root = 'C:\msys64'
)
$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $RepoRoot "build\$Config"
$OutDir = Join-Path $RepoRoot 'out'

if (-not (Test-Path "$Msys2Root\opt\devkitpro\cmake\3DS.cmake")) {
    throw "devkitPro toolchain not found - run scripts\bootstrap.ps1 first"
}
if ($Clean -and (Test-Path $BuildDir)) { Remove-Item -Recurse -Force $BuildDir }
New-Item -ItemType Directory -Force $BuildDir, $OutDir | Out-Null

function ToMsys([string]$p) { '/' + $p.Substring(0, 1).ToLower() + ($p.Substring(2) -replace '\\', '/') }
$src = ToMsys $RepoRoot
$bld = ToMsys $BuildDir

$env:MSYSTEM = 'MSYS'
$env:CHERE_INVOKING = '1'
$script = @"
set -e
export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
cd '$bld'
if [ ! -f build.ninja ]; then
  cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=`$DEVKITPRO/cmake/3DS.cmake -DCMAKE_BUILD_TYPE=$Config '$src'
fi
cmake --build . --target $Target
"@
& "$Msys2Root\usr\bin\bash.exe" -lc $script
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }

# Collect artefacts.
Get-ChildItem $BuildDir -Recurse -Include *.3dsx, *.cia, *.cxi, *.elf, *.lst, *.map |
    Where-Object { $_.FullName -notmatch '\\CMakeFiles\\' } |
    ForEach-Object { Copy-Item $_.FullName $OutDir -Force }
Get-ChildItem $OutDir | Where-Object { $_.Extension -in '.3dsx', '.cia', '.cxi', '.elf' } |
    Select-Object Name, @{ n = 'MB'; e = { [math]::Round($_.Length / 1MB, 2) } }, LastWriteTime | Format-Table -AutoSize
