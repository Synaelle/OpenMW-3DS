# Builds an older revision as a SEPARATE app that installs next to the current one, to test something on an older
# milestone without touching the current install: its own HOME Menu entry ("OpenMW <Name>", its own title ID) and
# its own SD folder sdmc:/openmw-<Name>/ (config, saves, logs, cooked textures), sharing the game data in
# sdmc:/openmw/data/ (Morrowind.esm, Morrowind.bsa, sound3ds.bsa: only read).
#
# Nothing in this checkout changes: the revision is checked out in a git worktree next to it
# (..\openmw3ds-snapshots\<Name>, sharing .deps and .tools through junctions), and the SD-folder and title edits are made
# there, never committed. Delete a snapshot with -Remove: it unlinks the junctions first (removing the worktree
# directly could follow them into this checkout's .deps and .tools).
#
#   scripts\build-snapshot.ps1 -Rev 9cb028b -Name m7 -TexturePack out\cooked\textures-before-m8.o3tp
#   scripts\build-snapshot.ps1 -Name m7 -Remove
#
# -TexturePack: a textures.o3tp cooked by the revision's own tools\cook_textures.py (the format changed with M8);
# without it, the revision's cooker makes one from the Morrowind data (read only).
# -AllowDepsChange: build even though deps/ or cmake/ changed since the revision, when the change only added things
# (check with git diff <rev> HEAD -- deps cmake): the worktree uses this checkout's .deps.
# Output: out\snapshots\<Name>\ - copy its contents to the SD card root, install cias\openmw3ds-<Name>.cia.
param(
    [string]$Rev = '',
    [Parameter(Mandatory)][ValidatePattern('^[a-z0-9]{1,8}$')][string]$Name,
    [string]$TexturePack = '',
    [string]$Start = "Seyda Neen, Arrille's Tradehouse",
    [switch]$AllowDepsChange,
    [switch]$Remove
)
$ErrorActionPreference = 'Stop'
$Main = Split-Path -Parent $PSScriptRoot
$Wt = Join-Path (Split-Path -Parent $Main) "openmw3ds-snapshots\$Name"
$Root = "openmw-$Name"
# Title IDs 0xF6D40-0xF6D7F for snapshots (0xF6D3E is the current app, 0xF6D30/31 the test apps).
$sum = 0; foreach ($c in $Name.ToCharArray()) { $sum += [int]$c }
$UniqueId = '0x{0:X5}' -f (0xF6D40 + ($sum % 0x40))

if ($Remove) {
    foreach ($dir in '.deps', '.tools') {
        $link = Join-Path $Wt $dir
        if (Test-Path $link) { [IO.Directory]::Delete($link, $false) } # the junction only, never its target
    }
    if (Test-Path $Wt) {
        git -C $Main worktree remove --force $Wt
        if ($LASTEXITCODE -ne 0) { throw 'git worktree remove failed' }
    }
    $pkg = Join-Path $Main "out\snapshots\$Name"
    if (Test-Path $pkg) { Remove-Item -Recurse -Force $pkg }
    Write-Host "Snapshot $Name removed"
    return
}
if (-not $Rev) { throw '-Rev is required' }

$commit = (git -C $Main rev-parse --verify "$Rev^{commit}").Trim()
if ($LASTEXITCODE -ne 0) { throw "unknown revision $Rev" }
# The worktree uses this checkout's prebuilt dependencies: refuse if they changed since the revision.
git -C $Main diff --quiet $commit HEAD -- deps cmake scripts/build-deps.ps1
if ($LASTEXITCODE -ne 0 -and -not $AllowDepsChange) {
    throw "deps/ or cmake/ changed since ${Rev}: the shared .deps may not match it (-AllowDepsChange if only additions)"
}
function FromMain([string]$path) { if ([IO.Path]::IsPathRooted($path)) { $path } else { Join-Path $Main $path } }

if (Test-Path $Wt) {
    $head = (git -C $Wt rev-parse HEAD).Trim()
    if ($head -ne $commit) { throw "$Wt holds $head, not $commit - remove it first (-Remove)" }
} else {
    New-Item -ItemType Directory -Force (Split-Path -Parent $Wt) | Out-Null
    git -C $Main worktree add --detach $Wt $commit | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'git worktree add failed' }
}
# The prebuilt dependencies and the packaging tools (makerom, bannertool) are git-ignored: shared by junctions.
foreach ($dir in '.deps', '.tools') {
    if (-not (Test-Path "$Wt\$dir")) { New-Item -ItemType Junction -Path "$Wt\$dir" -Target "$Main\$dir" | Out-Null }
}

# The SD folder and the title, edited in the worktree (each edit only matches the original text, so re-running
# applies what an earlier version of this script missed). Both spellings of the SD folder: "sdmc:/openmw/..." and
# the POSIX "/openmw/..." (TMPDIR, HOME, OpenMW's log folder); the game data stays in /openmw/data (openmw.cfg).
$utf8 = New-Object Text.UTF8Encoding($false)
function Edit([string]$path, [string]$pattern, [string]$replacement) {
    $text = [IO.File]::ReadAllText($path, $utf8)
    $new = [regex]::Replace($text, $pattern, $replacement)
    if ($new -ne $text) { [IO.File]::WriteAllText($path, $new, $utf8) }
}
function Expect([string]$path, [string]$literal) {
    if (-not ([IO.File]::ReadAllText($path, $utf8)).Contains($literal)) { throw "edit missing in ${path}: $literal" }
}
$dirs = @("$Wt\openmw\apps\openmw", "$Wt\openmw\components", "$Wt\platform3ds")
Get-ChildItem $dirs -Recurse -File -Include *.cpp, *.hpp, *.h, *.c | ForEach-Object {
    Edit $_.FullName 'sdmc:/openmw(?=[/"])' "sdmc:/$Root"
    Edit $_.FullName '"/openmw(?=[/"])' "`"/$Root"
}
$cmake = "$Wt\openmw\apps\openmw\CMakeLists.txt"
$mainCpp = "$Wt\openmw\apps\openmw\n3ds\main3ds.cpp"
Edit $cmake 'UNIQUE_ID 0xF6D3E' "UNIQUE_ID $UniqueId"
Edit $cmake 'TITLE "OpenMW"' "TITLE `"OpenMW $Name`""
Edit $mainCpp 'ciaUniqueId = 0xF6D3E' "ciaUniqueId = $UniqueId"
Expect "$Wt\openmw\components\files\threedspath.cpp" "sRoot = `"/$Root`""
Expect $cmake "UNIQUE_ID $UniqueId"
Expect $cmake "TITLE `"OpenMW $Name`""
Expect $mainCpp "ciaUniqueId = $UniqueId"
$left = Get-ChildItem $dirs -Recurse -File -Include *.cpp, *.hpp, *.h, *.c |
    Select-String -Pattern 'sdmc:/openmw[/"]|"/openmw[/"]' | Where-Object { $_.Path -notmatch '\\(launcher|opencs|components_tests)\\' }
if ($left) { throw "paths to sdmc:/openmw left:`n$($left -join "`n")" }

# Cooked data: the texture pack given (else cooked by the revision's tools), the sound archive from this checkout
# (the package doesn't ship it: the snapshot reads sdmc:/openmw/data/sound3ds.bsa).
New-Item -ItemType Directory -Force "$Wt\out\cooked" | Out-Null
if ($TexturePack -and -not (Test-Path "$Wt\out\cooked\textures.o3tp")) {
    New-Item -ItemType HardLink -Path "$Wt\out\cooked\textures.o3tp" -Target (FromMain $TexturePack) | Out-Null
}
if (-not (Test-Path "$Wt\out\cooked\sound3ds.bsa") -and (Test-Path "$Main\out\cooked\sound3ds.bsa")) {
    New-Item -ItemType HardLink -Path "$Wt\out\cooked\sound3ds.bsa" -Target "$Main\out\cooked\sound3ds.bsa" | Out-Null
}

# Build and package with the revision's own scripts (debug build, as its test packages were).
$make = "$Wt\scripts\make-sdcard-openmw.ps1"
$makeArgs = @{}
if ((Get-Command $make).Parameters.ContainsKey('Start')) { $makeArgs.Start = $Start }
& $make @makeArgs | Out-Host

$Pkg = Join-Path $Main "out\snapshots\$Name"
if (Test-Path $Pkg) { Remove-Item -Recurse -Force $Pkg }
New-Item -ItemType Directory -Force "$Pkg\cias" | Out-Null
$src = "$Wt\out\sdcard-openmw"
Copy-Item -Recurse "$src\openmw" "$Pkg\$Root"
Get-ChildItem "$Pkg\$Root\data" -Force | Remove-Item -Recurse -Force # shared: sdmc:/openmw/data
foreach ($f in Get-ChildItem "$Pkg\$Root" -File -Filter *.txt) {
    $text = [IO.File]::ReadAllText($f.FullName)
    if ($text.Contains('sdmc:/openmw/')) { [IO.File]::WriteAllText($f.FullName, $text.Replace('sdmc:/openmw/', "sdmc:/$Root/")) }
}
Copy-Item "$src\cias\openmw3ds.cia" "$Pkg\cias\openmw3ds-$Name.cia"

$buildId = (Select-String -Path "$Pkg\cias\openmw3ds-$Name.cia" -Pattern '20\d{6}-\d{6} [0-9a-f]{7}' -List).Matches[0].Value
@"
OpenMW 3DS - snapshot "$Name" (revision $commit, build $buildId)
=====================================================================
An older version of the port that installs NEXT TO the current one, to test something on it. It changes nothing
of the current install:
- HOME Menu: a second app, "OpenMW $Name" (title ID $UniqueId), same icon. Uninstall it with FBI when done.
- SD card: its own folder sdmc:/$Root/ (settings, saves, logs, cooked textures). It reads the game data from
  sdmc:/openmw/data/ (Morrowind.esm, Morrowind.bsa, sound3ds.bsa), which must be there already.

Install: copy the CONTENTS of this folder to the SD card root (it adds $Root/ and cias/openmw3ds-$Name.cia),
then install cias/openmw3ds-$Name.cia with FBI.
Logs: sdmc:/$Root/logs/. Remove: uninstall the app with FBI and delete sdmc:/$Root/.
(The matching openmw.elf, for crash reports, stays in $Wt\build\openmw.)
"@ | Set-Content "$Pkg\README.txt"

Write-Host "Snapshot package: $Pkg"
Get-ChildItem $Pkg -Recurse -File | Measure-Object Length -Sum | ForEach-Object { "{0} files, {1:N1} MB" -f $_.Count, ($_.Sum / 1MB) }
