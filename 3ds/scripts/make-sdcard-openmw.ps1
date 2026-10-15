# Builds out\sdcard-openmw\: an OpenMW hardware test package to copy onto the root of the 3DS SD card.
#   scripts\make-sdcard-openmw.ps1 [-NoBuild] [-Start "Seyda Neen, Arrille's Tradehouse"] [-BulletDouble] [-NoDebug]
# Morrowind's data files are NOT included: the tester copies Morrowind.esm and Morrowind.bsa from
# their own installation into sdmc:/openmw/data/ (see the README in the package).
param(
    [switch]$NoBuild,
    # Arrille's: the Census office's first door into it is the jail's PrisonMarker, whose destination is outside
    # the walls, so --start dropped the player into the void (OpenMW behaviour, see PORTING_NOTES).
    [string]$Start = "Seyda Neen, Arrille's Tradehouse",
    # Double-precision Bullet (upstream) instead of single precision: see build-openmw.ps1.
    [switch]$BulletDouble,
    # Release build: no debug tools (see build-openmw.ps1).
    [switch]$NoDebug
)
$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
if (-not $NoBuild) { & "$PSScriptRoot\build-openmw.ps1" -BulletDouble:$BulletDouble -NoDebug:$NoDebug | Out-Host }

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
This package's build: "build $shortBuild" at the top of the bottom screen (log: $buildId).
If the bottom screen shows another build, the new CIA is not the one installed.
What this tests (hardware test #25, M7 started: SOUND):
  Music, sound effects (footsteps, ambience, combat, spells, creatures) and NPC voices now play, in 3D
  (left/right follows where things are; muffled underwater).
  SOUND NEEDS THE 3DS'S DSP FIRMWARE: the file sdmc:/3ds/dspfirm.cdc. If you have ever played homebrew
  with sound, you most likely have it. If it's missing, run the DSP1 homebrew once (it creates that
  file). Without it the game runs silent and the log says why.
  Morrowind's Sound and Music folders are in the package, packed into one file:
  openmw/data/sound3ds.bsa (~240 MB, made from your own game files). Do NOT copy the loose Sound and
  Music folders to the SD card: they'd override it and slow every startup by several seconds.
  The bottom-screen menus (test #24) and the test character (5000 gold, attributes 75, skills 50, steel
  axe, Fire Bite and Fireball) are unchanged.
OpenMW starts a new game in "$Start"; the front door leads OUTSIDE into Seyda Neen. Still missing:
the HUD and graphical menus (M8).

1. Every test build: copy the CONTENTS of this folder to the ROOT of your SD card, merging folders and
   overwriting files (the whole openmw folder + cias/openmw3ds.cia). Your Morrowind files in
   sdmc:/openmw/data/ are not in the package, so they stay. The biggest files: openmw/data/sound3ds.bsa
   (~240 MB) and openmw/cooked/textures.o3tp (~70 MB). (out\sdcard-openmw-update still lists only the
   changed files, if ever needed.)
2. Copy these two files from your Morrowind "Data Files" folder into sdmc:/openmw/data/ :
     Morrowind.esm   (~79 MB)
     Morrowind.bsa   (~310 MB)
   (Nothing else is needed. The files are only read, never modified.)
3. Install cias/openmw3ds.cia with FBI, then launch "OpenMW3DS" from the HOME Menu.
4. While loading, the top screen is black (dark) and the bottom screen shows "(loading)", then FPS,
   cell, position and so on once the game runs. Loading takes under a minute.
   ALIVE INDICATOR = the last line of the bottom screen, redrawn every second:
     |   245s counting refs   idle   0s  53.0MB
     ^spinner ^uptime ^current step  ^seconds without progress  ^memory
   - spinner turning + uptime counting = the console is NOT frozen, just working.
   - "idle" green < 10 s, yellow < 30 s, red after. At 60 s the watchdog writes logs/openmw3ds-crash.log
     and returns to HOME by itself.
   - the spinner/uptime stop moving = real freeze (then force the power off).
   HOME works during loading too (Close exits immediately).
   The first two game frames are drawn slowly on purpose: the renderer submits each object separately
   and waits for the GPU, so that a GPU problem is written to the log instead of freezing the console.
5. Controls (all buttons are game controls now; the old D-pad turn and SELECT screenshot are gone):
     circle pad   walk (push past halfway to run)      C-stick   look
     ZR           attack / cast                         ZL        jump
     L (hold)     sneak                                 R         1st / 3rd person
     A            activate (talk, pick up, open doors)  B         inventory, and closes any menu
     X            ready weapon (fists if none)          Y         ready magic (select a spell first:
                                                                  B, then [Magic])
     D-pad up     journal                               D-pad down  rest / wait
     SELECT       quick save                            START     game menu
     hold X + ZL/ZR   previous/next weapon              hold Y + ZL/ZR   previous/next spell
   Bottom screen, top to bottom: build, FPS, cell, position and view direction, "look" (the object in
   reach under the crosshair), then a state line: "weapon ready / sneaking / 3rd person", "menu: ..."
   or "DEAD". While a menu is open it takes the middle of the bottom screen: touch a button to choose,
   touch the text to scroll, [<] [>] for more options.
   Touch the bar near the bottom ("tap: SCREENSHOT") to save a screenshot of the top screen to
   sdmc:/openmw/screenshots/ (under a second).
   Things to try, in one session (listen with headphones if you can; the volume slider works as usual):
   - Inside Arrille's: the music should start within a few seconds. Talk to Arrille, walk around
     (footsteps), open the door.
   - Outside in Seyda Neen: waves and wind, the docks creaking, the silt strider's calls, people greeting
     you as you walk past. Turn around slowly: sounds should move from one ear to the other.
   - Combat: X then ZR swings the axe (whoosh, hits); fight a mudcrab on the shore; cast Fireball (select
     it with B -> [Magic], then Y and ZR): casting and explosion sounds.
   - Swim in the sea and dive (ZL to jump in, look down and walk): sounds should get muffled underwater.
   - Travel to Balmora with the silt strider: music and town ambience there; walk around town.
   - START: the music keeps playing in the game menu; save and load once.
   - HOME menu and back: the sound should stop and come back.
   Note: crackles or pops, music that stutters or stops (especially while the game loads a new area), sounds
   that come too late, are too loud or too quiet (voices vs music vs effects), or come from the wrong
   side. A new sound may start a fraction of a second late the very first time it plays (it's read from
   the SD card): that's expected, but tell me if it's noticeable.
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
