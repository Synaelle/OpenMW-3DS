# Uploads a packaged test's changed files (out\sdcard-openmw-update\UPDATE.txt) to the 3DS's SD card over Wi-Fi,
# in one baguette-cli call: one connection and one job, so the 3DS shows "file x of n" and the time left (a call per
# file reconnected, logged in and checked the card each time).
#
#   powershell -ExecutionPolicy Bypass -File scripts\upload-update.ps1
#
# Only the listed files are written; folders merge into the SD card's. Then checks the CIA's size on the card.
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$baguette = Join-Path $repo '.toolsaguetteaguette-cli.exe'
$root = Join-Path $repo 'out\sdcard-openmw'
$list = Join-Path $repo 'out\sdcard-openmw-update\UPDATE.txt'

$files = Get-Content $list | Where-Object { $_ -match '^\s+sdmc:/' } | ForEach-Object { $_.Trim().Substring(5) }
if ($files.Count -eq 0) { Write-Output 'Nothing to upload.'; exit 0 }

# The changed files in a folder shaped like the SD card.
$stage = Join-Path $env:TEMP 'openmw-sd-update'
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
foreach ($f in $files) {
    $rel = $f.TrimStart('/') -replace '/', '\'
    $dst = Join-Path $stage $rel
    New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
    Copy-Item (Join-Path $root $rel) $dst
}

$tops = Get-ChildItem $stage | ForEach-Object FullName
& $baguette --json --quiet put @tops sd:/ | Out-Null
if ($LASTEXITCODE -ne 0) { Write-Output "Upload failed ($LASTEXITCODE)."; exit $LASTEXITCODE }
Write-Output "Uploaded $($files.Count) files."

if ($files -contains '/cias/openmw3ds.cia') {
    $entries = ((& $baguette --json --quiet ls /cias) | ConvertFrom-Json).entries
    $onCard = ($entries | Where-Object name -eq 'openmw3ds.cia').size
    $local = (Get-Item (Join-Path $root 'cias\openmw3ds.cia')).Length
    if ($onCard -ne $local) { Write-Output "CIA size differs: card $onCard, local $local."; exit 1 }
    Write-Output "CIA on the card: $onCard bytes (matches)."
}
Remove-Item -Recurse -Force $stage
