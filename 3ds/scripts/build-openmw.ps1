# Configures and builds OpenMW for the 3DS from openmw/, using cmake/openmw-3ds-cache.cmake.
#   scripts\build-openmw.ps1                         # build\openmw, -Os
#   scripts\build-openmw.ps1 -Opt O2 -BuildDir build\openmw-o2
#   scripts\build-openmw.ps1 -Clean
#   scripts\build-openmw.ps1 -BulletDouble          # link double-precision Bullet (upstream) instead of single
# Needs the cross-built dependencies (scripts\build-deps.ps1). Outputs, in the build dir:
#   openmw.elf (keep it for symbolize.ps1), apps\openmw\openmw.{3dsx,cia,cxi}
param(
    [string]$BuildDir = 'build\openmw',
    [ValidateSet('O2', 'Os')]
    [string]$Opt = 'Os',
    [string]$Target = 'all',
    [switch]$Clean,
    # Bullet precision is chosen on every configure, so building without the switch goes back to single precision.
    [switch]$BulletDouble,
    [string]$Msys2Root = 'C:\msys64'
)
$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
$Build = Join-Path $RepoRoot $BuildDir

if (-not (Test-Path "$Msys2Root\opt\devkitpro\cmake\3DS.cmake")) {
    throw "devkitPro toolchain not found - run scripts\bootstrap.ps1 first"
}
if (-not (Test-Path (Join-Path $RepoRoot '.deps\3ds\lib\libosg.a'))) {
    throw "dependencies not built - run scripts\build-deps.ps1 first"
}
if (-not $BulletDouble -and -not (Test-Path (Join-Path $RepoRoot '.deps\3ds-bullet-float\lib\libBulletCollision.a'))) {
    throw "single-precision Bullet not built - run scripts\build-deps.ps1 bulletfloat (or build with -BulletDouble)"
}
$bulletDoubleFlag = if ($BulletDouble) { 'ON' } else { 'OFF' }
if ($Clean -and (Test-Path $Build)) { Remove-Item -Recurse -Force $Build }
New-Item -ItemType Directory -Force $Build | Out-Null

function ToMsys([string]$p) { '/' + $p.Substring(0, 1).ToLower() + ($p.Substring(2) -replace '\\', '/') }
$src = ToMsys $RepoRoot
$bld = ToMsys $Build
# -Os (default): .text 24.2 MB instead of 34.3 MB at -O2 (measured 2026-10-14). The image counts
# against the 178 MB region, so every MB of code is a MB less heap. Dependencies keep -O2.
$flags = "-g -$Opt -DNDEBUG"

$env:MSYSTEM = 'MSYS'
$env:CHERE_INVOKING = '1'
# Configure on every run: cheap, and applies a changed -Opt to an existing build dir.
$script = @"
set -e
export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
cd '$bld'
gen=''
[ -f build.ninja ] || gen='-G Ninja'
cmake `$gen -C '$src/cmake/openmw-3ds-cache.cmake' -DCMAKE_TOOLCHAIN_FILE=`$DEVKITPRO/cmake/3DS.cmake \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo '-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=$flags' '-DCMAKE_C_FLAGS_RELWITHDEBINFO=$flags' \
  -DOPENMW3DS_BULLET_DOUBLE=$bulletDoubleFlag \
  '$src/openmw' > /dev/null
cmake --build . --target $Target
"@
& "$Msys2Root\usr\bin\bash.exe" -lc $script
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }

$elf = Join-Path $Build 'openmw.elf'
if (Test-Path $elf) {
    & "$Msys2Root\opt\devkitpro\devkitARM\bin\arm-none-eabi-size.exe" -A $elf |
        Select-String '^\.(text|data|bss|rodata|ARM\.exidx|ARM\.extab) ' | ForEach-Object { $_.Line }
}
