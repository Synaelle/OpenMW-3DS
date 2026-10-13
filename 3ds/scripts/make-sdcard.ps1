# Builds out\sdcard\: a folder to copy onto the root of the 3DS SD card for hardware tests.
#   scripts\make-sdcard.ps1 [-NoBuild] [-Autorun "probes bench"]
param(
    [switch]$NoBuild,
    [string]$Autorun = 'probes bench'
)
$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
if (-not $NoBuild) { & "$PSScriptRoot\build.ps1" | Out-Host }

$out = Join-Path $RepoRoot 'out'
$sd = Join-Path $out 'sdcard'
if (Test-Path $sd) { Remove-Item -Recurse -Force $sd }
New-Item -ItemType Directory -Force "$sd\3ds\openmw3ds", "$sd\cias", "$sd\openmw" | Out-Null

Copy-Item "$out\testapp.3dsx" "$sd\3ds\openmw3ds\testapp.3dsx"
Copy-Item "$out\testapp.cia" "$sd\cias\openmw3ds-testapp.cia"
if ($Autorun) { Set-Content "$sd\openmw\autorun.txt" $Autorun -NoNewline }

# Keep the matching .elf next to the package (NOT copied to the SD card) for crash symbolization.
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
Copy-Item "$out\testapp.elf" "$out\testapp-$stamp.elf"

@"
OpenMW 3DS - hardware test package ($stamp)
===========================================
Copy the CONTENTS of this folder to the ROOT of your SD card (merge folders).

  3ds/openmw3ds/testapp.3dsx   -> Homebrew Launcher build
  cias/openmw3ds-testapp.cia   -> install with FBI, then launch from the HOME Menu
  openmw/autorun.txt           -> makes the app run probes + GPU bench and exit by itself

Test steps: docs/HARDWARE_TEST_M0.md in the repo. Short version:
 1. Launch the CIA (HOME Menu). Wait ~30 s until it exits. Copy sdmc:/openmw/ to a folder 'cia'.
 2. Launch the 3DSX (Homebrew Launcher). Wait until it exits. Copy sdmc:/openmw/ to a folder '3dsx'.
 3. Delete sdmc:/openmw/autorun.txt, launch the CIA, hold L+R and press X.
    Red screen = crash logger worked. Press START. Copy sdmc:/openmw/crash.log.
 4. Send the folders back.
"@ | Set-Content "$sd\README.txt"

Write-Host "SD card package: $sd"
Write-Host "Matching ELF kept as: $out\testapp-$stamp.elf"
Get-ChildItem $sd -Recurse -File | ForEach-Object { $_.FullName.Substring($sd.Length + 1) }
