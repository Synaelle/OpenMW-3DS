# Builds out\sdcard\: a folder to copy onto the root of the 3DS SD card for hardware tests.
#   scripts\make-sdcard.ps1 [-NoBuild] [-Autorun "probes bench"]
param(
    [switch]$NoBuild,
    [string]$Autorun = 'probes bench',
    [switch]$With3dsx  # hardware tests use the .cia (real 178 MB / 804 MHz settings); 3dsx is optional
)
$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
if (-not $NoBuild) { & "$PSScriptRoot\build.ps1" | Out-Host }

$out = Join-Path $RepoRoot 'out'
$sd = Join-Path $out 'sdcard'
if (Test-Path $sd) { Remove-Item -Recurse -Force $sd }
New-Item -ItemType Directory -Force "$sd\cias", "$sd\openmw" | Out-Null
if ($With3dsx) {
    New-Item -ItemType Directory -Force "$sd\3ds\openmw3ds" | Out-Null
    Copy-Item "$out\testapp.3dsx" "$sd\3ds\openmw3ds\testapp.3dsx"
}
Copy-Item "$out\testapp.cia" "$sd\cias\openmw3ds-testapp.cia"
if ($Autorun) { Set-Content "$sd\openmw\autorun.txt" $Autorun -NoNewline }

# Keep the matching .elf next to the package (NOT copied to the SD card) for crash symbolization.
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
New-Item -ItemType Directory -Force "$out\elf-archive" | Out-Null
Copy-Item "$out\testapp.elf" "$out\elf-archive\testapp-$stamp.elf"

@"
OpenMW 3DS - hardware test package ($stamp)
===========================================
Copy the CONTENTS of this folder to the ROOT of your SD card (merge folders).

  cias/openmw3ds-testapp.cia   -> install with FBI, launch "OpenMW3DS testapp" from the HOME Menu
  openmw/autorun.txt           -> makes the app run its tests by itself and exit (~30 s)

Before testing: delete old files in sdmc:/openmw/ (openmw3ds.log, probe.txt, openmw3ds-crash.log), and
uninstall the previous "OpenMW3DS testapp" in FBI if you installed one.

1. Launch the CIA. The bottom screen shows progress (PROBE lines, then BENCH lines).
   It exits by itself when done.
2. If it freezes: note the LAST line on the bottom screen, power off, and send me
   sdmc:/openmw/openmw3ds.log (every line is now saved immediately, so it survives a freeze)
   plus anything in sdmc:/luma/dumps/arm11/.
3. If it finished: send sdmc:/openmw/openmw3ds.log and probe.txt.
   Optional crash-logger test: delete sdmc:/openmw/autorun.txt, launch again, hold L+R, press X.
"@ | Set-Content "$sd\README.txt"

Write-Host "SD card package: $sd"
Write-Host "Matching ELF kept as: $out\elf-archive\testapp-$stamp.elf"
Get-ChildItem $sd -Recurse -File | ForEach-Object { $_.FullName.Substring($sd.Length + 1) }
