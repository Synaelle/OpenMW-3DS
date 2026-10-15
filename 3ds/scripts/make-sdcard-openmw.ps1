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
What this tests (hardware test #24, M6 started: the game's MENUS, on the bottom screen):
  The game's windows (talking, inventory, trading, books, containers, resting, journal, the START
  menu...) now appear on the BOTTOM screen as text with touch buttons. The top screen keeps the game
  picture (paused while a menu is open). The real graphical menus come with M8; this is the playable
  version for now. How a menu looks:
     TITLE (yellow)
     text: touch its upper half to scroll back, its lower half to scroll on (^ / v show there is more)
     [<]   options 1-10 of 23   [>]    (only when there are more than 10: touch left/right side)
     buttons, two per row: the name, and below it a detail (price, count, weight, "equipped"...)
  Touch a button to choose it. B still closes most menus (and opens the inventory).
  TEST CHARACTER: every new game now starts with 5000 gold, all attributes at 75, all skills at 50, a
  steel axe (equipped: X readies it) and two fire spells, Fire Bite (touch) and Fireball (thrown): select
  one on the inventory's [Magic] page, then Y readies magic and ZR casts. (A console script run once at
  startup, openmw/test-start.txt; only in test builds.)
  Also: the TELEPORT buttons are gone (travel by silt strider or on foot now), and the build stamp at
  the top of the bottom screen is now Paris time, day/month first.
OpenMW starts a new game in "$Start"; the front door leads OUTSIDE into Seyda Neen. Still missing:
the HUD and graphical menus (M8), sound (M7).

1. Every test build: copy the CONTENTS of this folder to the ROOT of your SD card, merging folders and
   overwriting files (the whole openmw folder + cias/openmw3ds.cia). Your Morrowind files in
   sdmc:/openmw/data/ are not in the package, so they stay. openmw/cooked/textures.o3tp (~70 MB) is the
   biggest file. (out\sdcard-openmw-update still lists only the changed files, if ever needed.)
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
   or "DEAD". While a menu is open it takes the middle of the bottom screen (see above).
   Touch the bar near the bottom ("tap: SCREENSHOT") to save a screenshot of the top screen to
   sdmc:/openmw/screenshots/ (under a second).
   Things to try, in one session (save first with START -> Save game, so you can load if things go wrong):
   - Talk to Arrille (A): read his answers, try a few topics, then Barter: buy something cheap, sell
     something, Offer. Goodbye.
   - Inventory (B): open an item, Equip / Unequip, Drop one. [Magic]: select Fireball, close, then Y
     and ZR to cast it (at a wall, or at a mudcrab outside). X, then ZR swings the axe.
   - Pick up and read a book or a scroll (A on it): scroll the text, Take.
   - D-pad down: wait 1 hour. D-pad up: the journal. START: Save game, then Load game (pick your save).
   - Open a container nobody watches (taking things in front of people is a crime: the guard can kill a
     level-1 character; if you die, the bottom screen offers Load game). Take one item, then Take All.
   - Outside: walk to the silt strider by the docks, talk to the caravaner (Darvame Hleran) and Travel to
     Balmora. In Balmora: talk to a few people, try a trainer or a shop if you find one.
   - Any window with only a "Close" button is one I don't handle yet: note its title.
   Note anything unreadable, cut off, or buttons that don't react or do the wrong thing, and take a
   screenshot of the top screen when something looks wrong (the log says what the bottom screen showed).
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
