# Cross-builds OpenMW's dependencies into .deps\3ds (see deps/build-deps.sh).
#   scripts\build-deps.ps1              # build everything not built yet
#   scripts\build-deps.ps1 osg bullet   # (re)build specific components
$Components = $args
$Msys2Root = 'C:\msys64'
$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
$root = '/' + $RepoRoot.Substring(0, 1).ToLower() + ($RepoRoot.Substring(2) -replace '\\', '/')
$env:MSYSTEM = 'MSYS'
$env:CHERE_INVOKING = '1'
& "$Msys2Root\usr\bin\bash.exe" -lc "bash '$root/deps/build-deps.sh' $($Components -join ' ')"
if ($LASTEXITCODE -ne 0) { throw "dependency build failed ($LASTEXITCODE)" }
