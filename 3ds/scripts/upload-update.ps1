# Uploads a packaged test's changed files (out\sdcard-openmw-update\UPDATE.txt) to the 3DS's SD card over Wi-Fi,
# in one baguette-cli call: one connection and one job, so the 3DS shows "file x of n" and the time left (a call per
# file reconnected, logged in and checked the card each time).
#
#   powershell -ExecutionPolicy Bypass -File scripts\upload-update.ps1
#
# Only the listed files are written, folders merge into the SD card's, and the files the package no longer has are
# deleted (only under openmw/ and cias/, as listed). Then checks the CIA's size on the card.
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$baguette = Join-Path $repo '.tools\baguette\baguette-cli.exe'
$root = Join-Path $repo 'out\sdcard-openmw'
$list = Join-Path $repo 'out\sdcard-openmw-update\UPDATE.txt'

# UPDATE.txt: "Changed files (n):" then the files to send, and maybe "Delete these files from the SD card (n):" then
# the files the package no longer has (removed after the upload).
$files = @()
$deletes = @()
$section = ''
foreach ($line in Get-Content $list) {
    if ($line -match '^Changed files') { $section = 'put'; continue }
    if ($line -match '^Delete these files') { $section = 'rm'; continue }
    if ($line -notmatch '^\s+sdmc:/') { continue }
    $path = $line.Trim().Substring(5)
    if ($section -eq 'put') { $files += $path } elseif ($section -eq 'rm') { $deletes += $path }
}
if ($files.Count -eq 0 -and $deletes.Count -eq 0) { Write-Output 'Nothing to upload.'; exit 0 }

# The changed files in a folder shaped like the SD card.
$stage = Join-Path $env:TEMP 'openmw-sd-update'
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
foreach ($f in $files) {
    $rel = $f.TrimStart('/') -replace '/', '\'
    $dst = Join-Path $stage $rel
    New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
    Copy-Item (Join-Path $root $rel) $dst
}

if ($files.Count -gt 0) {
    $tops = @(Get-ChildItem $stage | ForEach-Object FullName) # an array even for one folder (@ would splat a string's characters)
    & $baguette --json --quiet put @tops sd:/ | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Output "Upload failed ($LASTEXITCODE)."; exit $LASTEXITCODE }
    Write-Output "Uploaded $($files.Count) files."
}
if ($deletes.Count -gt 0) {
    $targets = @($deletes | ForEach-Object { "sd:$_" })
    & $baguette --json --quiet rm @targets | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Output "Deleting failed ($LASTEXITCODE)."; exit $LASTEXITCODE }
    Write-Output "Deleted $($deletes.Count) files the package no longer has."
}

if ($files -contains '/cias/openmw3ds.cia') {
    $entries = ((& $baguette --json --quiet ls /cias) | ConvertFrom-Json).entries
    $onCard = ($entries | Where-Object name -eq 'openmw3ds.cia').size
    $local = (Get-Item (Join-Path $root 'cias\openmw3ds.cia')).Length
    if ($onCard -ne $local) { Write-Output "CIA size differs: card $onCard, local $local."; exit 1 }
    Write-Output "CIA on the card: $onCard bytes (matches)."
}
Remove-Item -Recurse -Force $stage
