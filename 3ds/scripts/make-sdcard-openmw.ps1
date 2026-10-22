# Builds out\sdcard-openmw\: an OpenMW hardware test package to copy onto the root of the 3DS SD card.
#   scripts\make-sdcard-openmw.ps1 [-NoBuild] [-Start "Seyda Neen, Arrille's Tradehouse"] [-NoDebug]
# Morrowind's data files are NOT included: the tester copies Morrowind.esm and Morrowind.bsa from
# their own installation into sdmc:/openmw/data/ (see the README in the package).
param(
    [switch]$NoBuild,
    # Arrille's: the Census office's first door into it is the jail's PrisonMarker, whose destination is outside
    # the walls, so --start dropped the player into the void (OpenMW behaviour, see PORTING_NOTES).
    [string]$Start = "Seyda Neen, Arrille's Tradehouse",
    # Release build: no debug tools (see build-openmw.ps1).
    [switch]$NoDebug
)
$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
if (-not $NoBuild) { & "$PSScriptRoot\build-openmw.ps1" -NoDebug:$NoDebug | Out-Host }

$out = Join-Path $RepoRoot 'out'
$sd = Join-Path $out 'sdcard-openmw'
$app = Join-Path $RepoRoot 'build\openmw\apps\openmw'
if (-not (Test-Path "$app\openmw.cia")) { throw "no OpenMW CIA in $app - run scripts\build-openmw.ps1" }
if (Test-Path $sd) { Remove-Item -Recurse -Force $sd }
New-Item -ItemType Directory -Force "$sd\cias" | Out-Null

# Config, resources and an empty data folder (no junction, no game data).
& "$PSScriptRoot\stage-sd.ps1" -SdRoot $sd -DataMode none | Out-Null
Copy-Item "$app\openmw.cia" "$sd\cias\openmw3ds.cia"
# One argument per line (the cell name contains spaces).
$startArgs = @('--skip-menu', '--start', $Start)
if (-not $NoDebug) {
    # Test builds: a console script run once at startup gives the new character gold and skills at 50
    # (packaging/test-start.txt; the console's output goes to openmw.log).
    Copy-Item (Join-Path $RepoRoot 'packaging\test-start.txt') "$sd\openmw\test-start.txt"
    # The hardware test's runs, chosen on the bottom screen at startup (main3ds.cpp testMenu): it creates the
    # chosen run's switch files, so the tester never handles them.
    Copy-Item (Join-Path $RepoRoot 'packaging\test-menu.txt') "$sd\openmw\test-menu.txt"
    $startArgs += @('--script-run', 'sdmc:/openmw/test-start.txt')
}
[IO.File]::WriteAllLines("$sd\openmw\args.txt", [string[]]$startArgs)

# The matching .elf (with debug info, ~1.8 GB) stays on the PC for crash symbolization.
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
# The build stamp compiled into the CIA (p3ds_build_id, Paris time), shown on the bottom screen as
# "build DD/MM HH:MM".
$buildId = (Select-String -Path "$app\openmw.cia" -Pattern '20\d{6}-\d{6} [0-9a-f]{7}' -List).Matches[0].Value
$shortBuild = if ($buildId) {
    '{0}/{1} {2}:{3}' -f $buildId.Substring(6, 2), $buildId.Substring(4, 2), $buildId.Substring(9, 2), $buildId.Substring(11, 2)
} else { '?' }
New-Item -ItemType Directory -Force "$out\elf-archive" | Out-Null
$elf = Get-Item (Join-Path $RepoRoot 'build\openmw\openmw.elf')
$archived = Join-Path "$out\elf-archive" ('openmw-' + $elf.LastWriteTime.ToString('yyyyMMdd-HHmmss') + '.elf')
if (-not (Test-Path $archived)) { Copy-Item $elf.FullName $archived } # once per build, not per package

@"
OpenMW 3DS - OpenMW hardware test package ($stamp)
===================================================
This package's build: "build $shortBuild" at the top of the bottom screen while the game starts, and in
the Debug tab (log: $buildId). If it shows another build, the new CIA is not the one installed.
What this tests (hardware test #77: M10 phase 1, the interface's per-frame cost):
  The interface no longer redoes its work every frame when nothing on it changed, and the minimap moves ~4
  times a second (its arrow in 16 steps). The frame rate should be higher, mostly while walking outside. A short
  run is enough: Seyda Neen outside for 3-4 minutes, walking and looking around, then an interior; check the
  HUD still updates (health, magicka, fatigue, the minimap, messages). Also new since #76: every window not
  ported yet (spell buying, rest, level-up, journal, alchemy, repair, enchanting, companions...) opens on the top
  screen, used with the cursor. New: the CIA, openmw/test-menu.txt, openmw/ui3ds (layers, OMW3DS texts)
  (uploaded). Logs: logs/test77-A/.
  Note: Rosalina's menu combination (L + Down + SELECT) also reaches the game: sneak, rest ("Resting here is
  illegal") and quick save. That's harmless.

  As in test #26, the bottom screen is the game's own interface, in Morrowind's style:
  - the HUD: health, magicka and fatigue with their values, the weapon and the spell with their names,
    active effects, the minimap (still black: maps come in a later step) and the quick keys 1-9 (touch one
    to use it);
  - tabs at the top, which do NOT pause the game: Inventory, Magic, Stats, and Debug (the old debug lines:
    build, FPS, cell..., and a Screenshot button). Touch a tab to open it; the house at the left (or the
    open tab again) goes back to the HUD. Touching the bars, the weapon or the spell in the HUD opens their
    tab too.
  - top screen: the crosshair, and what it's on as one small line at the very top ("Wooden Door to Seyda
    Neen").
  Menus that pause the game (dialogue, containers, barter, books, journal, rest, START's game menu...) still
  use the old text menus on the bottom screen: they're the next step.
  Also new: closing from the HOME Menu exits immediately (no wait, no crash log on exit).
  If the new interface goes badly wrong: an empty file sdmc:/openmw/no-m8ui brings back the old bottom screen
  (delete it afterwards). The test character (5000 gold, attributes 75, skills 50, steel axe, Fire Bite and
  Fireball) and the sound are unchanged.
OpenMW starts a new game in "$Start"; the front door leads OUTSIDE into Seyda Neen.

1. Every test build: copy the CONTENTS of this folder to the ROOT of your SD card, merging folders and
   overwriting files (the whole openmw folder + cias/openmw3ds.cia). Your Morrowind files in
   sdmc:/openmw/data/ are not in the package, so they stay. The biggest files: openmw/data/sound3ds.bsa
   (~240 MB) and openmw/cooked/textures.o3tp (~70 MB). (out\sdcard-openmw-update still lists only the
   changed files, if ever needed.)
2. Copy these two files from your Morrowind "Data Files" folder into sdmc:/openmw/data/ :
     Morrowind.esm   (~79 MB)
     Morrowind.bsa   (~310 MB)
   (Nothing else: Sound and Music come packed in openmw/data/sound3ds.bsa, so don't copy those folders.
   The files are only read, never modified.)
   SOUND also needs the 3DS's DSP firmware, the file sdmc:/3ds/dspfirm.cdc (once per SD card, not per
   build). If you've played homebrew with sound, it's already there. If not, run the DSP1 homebrew once:
   it creates the file. Without it the game runs silent and logs/openmw3ds.log says
   "sound: the DSP didn't start".
3. Install cias/openmw3ds.cia with FBI, then launch "OpenMW3DS" from the HOME Menu.
4. While the game loads at startup, the top screen is black (dark) and the bottom screen shows "(loading)"
   and the old debug lines, then the new interface once the game runs. Loading takes under a minute.
   ALIVE INDICATOR (startup loading only) = the last line of the bottom screen, redrawn every second:
     |   245s counting refs   idle   0s  53.0MB
     ^spinner ^uptime ^current step  ^seconds without progress  ^memory
   - spinner turning + uptime counting = the console is NOT frozen, just working.
   - "idle" green < 10 s, yellow < 30 s, red after. At 60 s the watchdog writes logs/openmw3ds-crash.log
     and returns to HOME by itself.
   - the spinner/uptime stop moving = real freeze (then force the power off).
   HOME works during loading too (Close exits immediately). Later loads (doors, travel) show a "Loading"
   box on the bottom screen.
5. Controls (unchanged, except B):
     circle pad   walk (push past halfway to run)      C-stick   look
     ZR           attack / cast                         ZL        jump
     L (hold)     sneak                                 R         1st / 3rd person
     A            activate (talk, pick up, open doors)  B         Inventory tab (open / close), and closes
                                                                  any menu
     X            ready weapon (fists if none)          Y         ready magic (pick the spell in the Magic
                                                                  tab first)
     D-pad up     journal                               D-pad down  rest / wait
     SELECT       quick save                            START     game menu
     hold X + ZL/ZR   previous/next weapon              hold Y + ZL/ZR   previous/next spell
   Screenshots: L + D-pad LEFT (or Debug tab -> [Screenshot]) saves both screens to sdmc:/openmw/screenshots/
   (top_NNN.bmp, bottom_NNN.bmp and bottomgpu_NNN.bmp).
   Panic: L + R + START + SELECT held for a second quits the game.
   Things to try, in one session:
   - The HUD: do the bars, names and icons look right and read well? Is anything too small or cut off?
   - Inventory tab: the filters (All, Weapon, Apparel, Magic, Misc); touch an item: its name, weight and
     value, then Unequip/Equip, Drop (pick it up again with A), and Quick key -> a number: it shows in the
     HUD's quick-key row; touch it there to use it.
   - Magic tab: touch Fireball (it becomes the HUD's spell), then Y and ZR to cast. Put Fire Bite on a
     quick key.
   - Stats tab: scroll the skills list. Debug tab: note the FPS here and on the HUD.
   - Walk, fight and look around with a tab open: the game keeps running.
   - Talk to someone, open a container, START: the old text menus take the bottom screen, and the new
     interface comes back when they close.
   - Go outside (front door) and walk around Seyda Neen for a few minutes (see above).
   - HOME -> Close at the end: it should close right away.
   Note: anything that looks wrong or unpolished, touches that don't respond (or hit the wrong thing), the
   FPS compared to the last test, and what you like and don't like about the layout.
6. Send back the logs folder and your screenshots:
     sdmc:/openmw/logs/openmw3ds.log         the 3DS port log (timings, memory, platform)
     sdmc:/openmw/logs/openmw.log            OpenMW's own log
     sdmc:/openmw/logs/openmw3ds-crash.log   only if there is one
     sdmc:/openmw/screenshots/*.bmp          your screenshots
   (Older builds wrote sdmc:/openmw/openmw3ds.log, openmw3ds-crash.log, openmw.log, crash.log and
   sdmc:/openmw/config/openmw.log: those files can be deleted.)

If you have to force the power off anyway, still send logs/openmw3ds.log.
"@ | Set-Content "$sd\README.txt"

Write-Host "Package: $sd"
Get-ChildItem $sd -Recurse -File | Measure-Object Length -Sum | ForEach-Object { "{0} files, {1:N1} MB" -f $_.Count, ($_.Sum / 1MB) }

# Incremental update: out\sdcard-openmw-update\ holds only the files that differ from the previous
# package (copying the ~380 small files of resources/ over FTP is slow), and UPDATE.txt lists them.
# The manifest of the previous package is out\sdcard-openmw.manifest.json; -Full skips the diff.
$manifestPath = Join-Path $out 'sdcard-openmw.manifest.json'
$update = Join-Path $out 'sdcard-openmw-update'
$manifest = [ordered]@{}
Get-ChildItem $sd -Recurse -File | Where-Object { $_.Name -ne 'README.txt' } | Sort-Object FullName | ForEach-Object {
    $rel = $_.FullName.Substring($sd.Length + 1).Replace('\', '/')
    $manifest[$rel] = (Get-FileHash $_.FullName -Algorithm SHA256).Hash
}
$previous = @{}
if (Test-Path $manifestPath) {
    (Get-Content $manifestPath -Raw | ConvertFrom-Json).PSObject.Properties | ForEach-Object { $previous[$_.Name] = $_.Value }
}
$changed = @($manifest.Keys | Where-Object { $previous[$_] -ne $manifest[$_] })
$removed = @($previous.Keys | Where-Object { -not $manifest.Contains($_) } | Sort-Object)
if (Test-Path $update) { Remove-Item -Recurse -Force $update }
New-Item -ItemType Directory -Force $update | Out-Null
foreach ($rel in $changed) {
    $dst = Join-Path $update $rel
    New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
    Copy-Item (Join-Path $sd $rel) $dst
}
# The tester copies the whole openmw folder when it changed, and only installs the CIA when it didn't.
$openmwChanged = @($changed + $removed | Where-Object { $_ -like 'openmw/*' }).Count -gt 0
$verdict = if ($openmwChanged) { 'openmw folder: CHANGED - copy the whole openmw folder, then install the CIA' }
           else { 'openmw folder: UNCHANGED - only install the CIA' }
$lines = @("OpenMW 3DS - update ${stamp}: copy the CONTENTS of this folder to the SD card root (overwrite).", '',
    "Build: $buildId (bottom screen: build $shortBuild)", '',
    $verdict, '', "Changed files ($($changed.Count)):") + ($changed | ForEach-Object { "  sdmc:/$_" })
if ($removed.Count) { $lines += @('', "Delete these files from the SD card ($($removed.Count)):") + ($removed | ForEach-Object { "  sdmc:/$_" }) }
if (-not (Test-Path $manifestPath)) { $lines += @('', '(No previous manifest: this is the full package.)') }
$lines | Set-Content (Join-Path $update 'UPDATE.txt')
$manifest | ConvertTo-Json | Set-Content $manifestPath
Write-Host "Update: $update"
$lines | ForEach-Object { Write-Host $_ }

# Test archive (the tester, 19/10: "archive the whole environment for previous tests"): the whole SD
# environment of this package in out\test-archive\<test>\ (<test> from test-menu.txt's "logs" line), so an
# old test can be put back exactly with scripts\restore-test.ps1. Files are stored once by content in
# out\test-archive\.store and hard-linked: a test costs only the files it changed (the game data, ~230 MB,
# once). Packaging the same test again replaces its archive.
$testName = (Get-Content (Join-Path $sd 'openmw\test-menu.txt') | Where-Object { $_ -match '^logs\s+(\S+)' } |
    Select-Object -First 1) -replace '^logs\s+', ''
if ($testName) {
    $archiveRoot = Join-Path $out 'test-archive'
    $store = Join-Path $archiveRoot '.store'
    $archive = Join-Path $archiveRoot $testName
    New-Item -ItemType Directory -Force $store | Out-Null
    if (Test-Path $archive) { Remove-Item -Recurse -Force $archive }
    foreach ($rel in $manifest.Keys) {
        $stored = Join-Path $store $manifest[$rel]
        if (-not (Test-Path $stored)) { Copy-Item (Join-Path $sd $rel) $stored }
        $dst = Join-Path $archive ('sd\' + $rel.Replace('/', '\'))
        New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
        New-Item -ItemType HardLink -Path $dst -Target $stored | Out-Null
    }
    $manifest | ConvertTo-Json | Set-Content (Join-Path $archive 'manifest.json')
    @("Test: $testName", "Build: $buildId (bottom screen: build $shortBuild)", "Packaged: $stamp",
        "Program with symbols: $archived") | Set-Content (Join-Path $archive 'BUILD.txt')
    Write-Host "Archived: $archive"
}

# Old ELFs: stripped to their symbols or deleted (scripts\prune-elf-archive.ps1, the tester's local script, not
# in the repo).
$prune = Join-Path $PSScriptRoot 'prune-elf-archive.ps1'
if (Test-Path $prune) { & $prune | Out-Host }
