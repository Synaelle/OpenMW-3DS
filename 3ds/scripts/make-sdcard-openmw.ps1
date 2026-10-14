# Builds out\sdcard-openmw\: an OpenMW hardware test package to copy onto the root of the 3DS SD card.
#   scripts\make-sdcard-openmw.ps1 [-NoBuild] [-Start "Seyda Neen, Arrille's Tradehouse"]
# Morrowind's data files are NOT included: the tester copies Morrowind.esm and Morrowind.bsa from
# their own installation into sdmc:/openmw/data/ (see the README in the package).
param(
    [switch]$NoBuild,
    # Arrille's: the Census office's first door into it is the jail's PrisonMarker, whose destination is outside
    # the walls, so --start dropped the player into the void (OpenMW behaviour, see PORTING_NOTES).
    [string]$Start = "Seyda Neen, Arrille's Tradehouse"
)
$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
if (-not $NoBuild) { & "$PSScriptRoot\build-openmw.ps1" | Out-Host }

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
[IO.File]::WriteAllLines("$sd\openmw\args.txt", [string[]]@('--skip-menu', '--start', $Start))

# The matching .elf (with debug info, ~1.8 GB) stays on the PC for crash symbolization.
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
# The build stamp compiled into the CIA (p3ds_build_id, UTC), shown on the bottom screen as "build MMDD-HHMM".
$buildId = (Select-String -Path "$app\openmw.cia" -Pattern '20\d{6}-\d{6} [0-9a-f]{7}' -List).Matches[0].Value
$shortBuild = if ($buildId) { $buildId.Substring(4, 4) + '-' + $buildId.Substring(9, 4) } else { '?' }
New-Item -ItemType Directory -Force "$out\elf-archive" | Out-Null
$elf = Get-Item (Join-Path $RepoRoot 'build\openmw\openmw.elf')
$archived = Join-Path "$out\elf-archive" ('openmw-' + $elf.LastWriteTime.ToString('yyyyMMdd-HHmmss') + '.elf')
if (-not (Test-Path $archived)) { Copy-Item $elf.FullName $archived } # once per build, not per package

@"
OpenMW 3DS - OpenMW hardware test package ($stamp)
===================================================
This package's build: "build $shortBuild" at the top of the bottom screen (log: $buildId).
If the bottom screen shows another build, the new CIA is not the one installed.
What this tests: EXTERIORS (milestone M5), with the fix from hardware test #19: the half-missing,
flashing water (and any other geometry cut off close to the camera). Fires, particles and background
loading as in the previous build. OpenMW starts a new game in "$Start"; the front door leads OUTSIDE
into Seyda Neen. People may still cause a short stall when they appear. Still missing: menus/HUD (M8),
sound (M7).

1. Every test build: copy the CONTENTS of this folder to the ROOT of your SD card, merging folders and
   overwriting files (the whole openmw folder + cias/openmw3ds.cia). Your Morrowind files in
   sdmc:/openmw/data/ are not in the package, so they stay. openmw/cooked/textures.o3tp (~70 MB) is the
   biggest file. (out\sdcard-openmw-update still lists only the changed files, if ever needed.)
2. Copy these two files from your Morrowind "Data Files" folder into sdmc:/openmw/data/ :
     Morrowind.esm   (~79 MB)
     Morrowind.bsa   (~310 MB)
   (Nothing else is needed. The files are only read, never modified.)
3. Install cias/openmw3ds.cia with FBI, then launch "OpenMW3DS" from the HOME Menu.
4. While loading, the top screen is black (dark) and the bottom screen shows the debug display:
   "(loading)" and log lines, then FPS, draw calls and triangles once the game runs. Loading takes ~1.5 min.
   ALIVE INDICATOR = the last line of the bottom screen, redrawn every second:
     |   245s counting refs   idle   0s  53.0MB
     ^spinner ^uptime ^current step  ^seconds without progress  ^memory
   - spinner turning + uptime counting = the console is NOT frozen, just working.
   - "idle" green < 10 s, yellow < 30 s, red after. At 60 s the watchdog writes openmw3ds-crash.log and
     returns to HOME by itself.
   - the spinner/uptime stop moving = real freeze (then force the power off).
   HOME works during loading too (Close exits immediately).
   The first two game frames are drawn slowly on purpose: the renderer submits each object separately
   and waits for the GPU, so that a GPU problem is written to the log instead of freezing the console.
5. Controls (all buttons are game controls now; the old D-pad turn and SELECT screenshot are gone):
     circle pad   walk (push past halfway to run)      C-stick   look
     ZR           attack / cast                         ZL        jump
     L (hold)     sneak                                 R         1st / 3rd person
     A            activate (talk, pick up, open doors)  B         inventory, and closes any menu
     X            ready weapon (fists if none)          Y         ready magic (needs a selected spell:
                                                                  none yet, so it does nothing for now)
     D-pad up     journal                               D-pad down  rest / wait
     SELECT       quick save                            START     game menu
     hold X + ZL/ZR   previous/next weapon              hold Y + ZL/ZR   previous/next spell
   Touch the bar at the bottom of the bottom screen ("tap: SCREENSHOT") to save a screenshot of the top
   screen to sdmc:/openmw/screenshots/ (takes ~2 s).
   Bottom screen: HP/MP/FT, then a state line: "weapon ready / sneaking / 3rd person", or in yellow
   "menu: inventory - not drawn, B closes" when the game opened a menu you can't see yet, or "DEAD".
   Game messages ("Saving...", "You stole...") appear in the log lines below.
   Menus: the top-screen picture keeps running, but the game is paused behind an invisible menu: press B.
   Things to try: go out of the front door (A). Loading outside takes ~10 s. Walk around Seyda Neen:
   the docks and the sea, the houses, the lighthouse direction. Look at the sky. Go into one or two
   houses and back out. Walk out of town a bit (each new area loads for a few seconds). Note anything
   that looks wrong (missing objects, wrong colours, things popping in too close), and the FPS on the
   bottom screen. Take a few screenshots (tap the bar). Then HOME -> Close.
6. Send back these logs:
     sdmc:/openmw/openmw3ds.log          the 3DS port log (timings, memory, platform)
     sdmc:/openmw/config/openmw.log      OpenMW's own log
     sdmc:/openmw/openmw3ds-crash.log    only if there is one
     sdmc:/openmw/screenshots/*.bmp      your screenshots
   (Older test builds wrote sdmc:/openmw/openmw.log and crash.log: those files can be deleted.)

If you have to force the power off anyway, still send openmw3ds.log.
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
