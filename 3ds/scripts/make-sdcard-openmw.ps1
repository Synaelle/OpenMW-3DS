# Builds out\sdcard-openmw\: an OpenMW hardware test package to copy onto the root of the 3DS SD card.
#   scripts\make-sdcard-openmw.ps1 [-NoBuild] [-Start "Seyda Neen, Census and Excise Office"]
# Morrowind's data files are NOT included: the tester copies Morrowind.esm and Morrowind.bsa from
# their own installation into sdmc:/openmw/data/ (see the README in the package).
param(
    [switch]$NoBuild,
    [string]$Start = 'Seyda Neen, Census and Excise Office'
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
Copy-Item (Join-Path $RepoRoot 'build\openmw\openmw.elf') "$out\elf-archive\openmw-$stamp.elf"

@"
OpenMW 3DS - OpenMW hardware test package ($stamp)
===================================================
What this tests: OpenMW itself, headless (NO picture on the top screen yet - the 3DS renderer is the
next milestone). It loads Morrowind.esm, starts a new game in "$Start" and runs the game loop.
We want: loading time, memory, and the real frame time without rendering.

1. Copy the CONTENTS of this folder to the ROOT of your SD card (merge folders).
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
   - "idle" green < 10 s, yellow < 30 s, red after. At 60 s the watchdog writes crash.log and
     returns to HOME by itself.
   - the spinner/uptime stop moving = real freeze (then force the power off).
   HOME works during loading too now (Close exits immediately).
5. Once the bottom screen shows the cell name and an FPS value, let it run ~1 minute, then press
   HOME and close the software.
6. Send back sdmc:/openmw/openmw.log, and sdmc:/openmw/crash.log if there is one.

If you have to force the power off anyway, still send openmw.log.
"@ | Set-Content "$sd\README.txt"

Write-Host "Package: $sd"
Get-ChildItem $sd -Recurse -File | Measure-Object Length -Sum | ForEach-Object { "{0} files, {1:N1} MB" -f $_.Count, ($_.Sum / 1MB) }
