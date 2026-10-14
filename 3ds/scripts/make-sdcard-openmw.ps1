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
New-Item -ItemType Directory -Force "$out\elf-archive" | Out-Null
$elf = Get-Item (Join-Path $RepoRoot 'build\openmw\openmw.elf')
$archived = Join-Path "$out\elf-archive" ('openmw-' + $elf.LastWriteTime.ToString('yyyyMMdd-HHmmss') + '.elf')
if (-not (Test-Path $archived)) { Copy-Item $elf.FullName $archived } # once per build, not per package

@"
OpenMW 3DS - OpenMW hardware test package ($stamp)
===================================================
What this tests: OpenMW itself, headless (NO picture on the top screen yet - the 3DS renderer is the
next milestone). It loads Morrowind.esm, starts a new game in "$Start" and runs the game loop.
We want: loading time, memory, and the real frame time without rendering.

1. First time: copy the CONTENTS of this folder to the ROOT of your SD card (merge folders).
   Later test builds come as a small update folder (sdcard-openmw-update) with only the changed
   files, listed in its UPDATE.txt: copy that instead, overwriting.
2. Copy these two files from your Morrowind "Data Files" folder into sdmc:/openmw/data/ :
     Morrowind.esm   (~79 MB)
     Morrowind.bsa   (~310 MB)
   (Nothing else is needed. The files are only read, never modified.)
3. Install cias/openmw3ds.cia with FBI, then launch "OpenMW3DS" from the HOME Menu.
4. The top screen stays black. The bottom screen shows the debug display: "(loading)" and log lines
   while loading, then FPS and the cell name once the game loop runs. Loading takes minutes.
   ALIVE INDICATOR = the last line of the bottom screen, redrawn every second:
     |   245s counting refs   idle   0s  53.0MB
     ^spinner ^uptime ^current step  ^seconds without progress  ^memory
   - spinner turning + uptime counting = the console is NOT frozen, just working.
   - "idle" green < 10 s, yellow < 30 s, red after. At 60 s the watchdog writes openmw3ds-crash.log and
     returns to HOME by itself.
   - the spinner/uptime stop moving = real freeze (then force the power off).
   HOME works during loading too now (Close exits immediately).
5. Once the bottom screen shows the cell name and an FPS value, let it run ~1 minute, then press
   HOME and close the software.
6. Send back these logs:
     sdmc:/openmw/openmw3ds.log          the 3DS port log (timings, memory, platform)
     sdmc:/openmw/config/openmw.log      OpenMW's own log
     sdmc:/openmw/openmw3ds-crash.log    only if there is one
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
$lines = @("OpenMW 3DS - update ${stamp}: copy the CONTENTS of this folder to the SD card root (overwrite).", '',
    "Changed files ($($changed.Count)):") + ($changed | ForEach-Object { "  sdmc:/$_" })
if ($removed.Count) { $lines += @('', "Delete these files from the SD card ($($removed.Count)):") + ($removed | ForEach-Object { "  sdmc:/$_" }) }
if (-not (Test-Path $manifestPath)) { $lines += @('', '(No previous manifest: this is the full package.)') }
$lines | Set-Content (Join-Path $update 'UPDATE.txt')
$manifest | ConvertTo-Json | Set-Content $manifestPath
Write-Host "Update: $update"
$lines | ForEach-Object { Write-Host $_ }
