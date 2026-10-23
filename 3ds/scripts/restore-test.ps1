# Puts an archived test's whole SD environment back on the 3DS over Wi-Fi (baguette-cli), from
# out\test-archive\<test> (written by make-sdcard-openmw.ps1 for every package).
#   scripts\restore-test.ps1 -Test test77          # the files that differ from what the SD holds now
#   scripts\restore-test.ps1 -Test test77 -List    # only say what would change
# What the SD holds is out\sdcard-openmw.manifest.json (the last package, or the last restore): only the files
# that differ are sent, files the test did not have are deleted (under openmw/ and cias/ only), and the manifest
# then records the restored test, so the next package's update list is right. The tester then installs the CIA.
# Tests packaged since 22/10/2026 live in sdmc:/openmw-test with the "OpenMW Test" title. Older ones used
# sdmc:/openmw and the player's title: restoring one would write into the player's install and its CIA would
# replace their OpenMW, so they need -IntoPlayerInstall.
param(
    [Parameter(Mandatory)][string]$Test,
    [switch]$List,
    [switch]$IntoPlayerInstall
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root 'out'
$archive = Join-Path $out "test-archive\$Test"
if (-not (Test-Path "$archive\manifest.json")) {
    $known = (Get-ChildItem (Join-Path $out 'test-archive') -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -ne '.store' }).Name -join ', '
    throw "No archive for '$Test' (archived: $known)"
}
$baguette = Join-Path $root '.tools\baguette\baguette-cli.exe'
$sdManifestPath = Join-Path $out 'sdcard-openmw.manifest.json'

$wanted = @{}
(Get-Content "$archive\manifest.json" -Raw | ConvertFrom-Json).PSObject.Properties | ForEach-Object { $wanted[$_.Name] = $_.Value }
$onSd = @{}
if (Test-Path $sdManifestPath) {
    (Get-Content $sdManifestPath -Raw | ConvertFrom-Json).PSObject.Properties | ForEach-Object { $onSd[$_.Name] = $_.Value }
}
$send = @($wanted.Keys | Where-Object { $onSd[$_] -ne $wanted[$_] } | Sort-Object)
if (-not $IntoPlayerInstall -and @($wanted.Keys | Where-Object { $_ -like 'openmw/*' }).Count -gt 0) {
    throw "$Test predates the separate test install: it writes to sdmc:/openmw and its CIA replaces the player's OpenMW. Add -IntoPlayerInstall to restore it anyway."
}
# Only the test install and test CIAs are cleaned (the player's sdmc:/openmw too with -IntoPlayerInstall).
$delete = @($onSd.Keys | Where-Object {
        -not $wanted.ContainsKey($_) -and ($_ -like 'openmw-test/*' -or $_ -like 'cias/*-test.cia' -or
            ($IntoPlayerInstall -and ($_ -like 'openmw/*' -or $_ -like 'cias/*')))
    } | Sort-Object)

Get-Content "$archive\BUILD.txt" | ForEach-Object { Write-Host $_ }
Write-Host "Send ($($send.Count)):"; $send | ForEach-Object { Write-Host "  sdmc:/$_" }
if ($delete.Count) { Write-Host "Delete ($($delete.Count)):"; $delete | ForEach-Object { Write-Host "  sdmc:/$_" } }
if ($List) { return }

function Invoke-Baguette([string[]]$arguments) {
    & $baguette --json --quiet @arguments | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "baguette-cli $($arguments -join ' ') failed (exit $LASTEXITCODE)" }
}
foreach ($rel in $send) {
    $dir = Split-Path $rel -Parent
    Invoke-Baguette @('put', (Join-Path "$archive\sd" $rel.Replace('/', '\')), "sd:/$($dir.Replace('\', '/'))/")
    $onSd[$rel] = $wanted[$rel] # recorded per file: an interrupted restore can be run again
    $onSd | ConvertTo-Json | Set-Content $sdManifestPath
}
foreach ($rel in $delete) {
    Invoke-Baguette @('rm', "sd:/$rel")
    $onSd.Remove($rel)
    $onSd | ConvertTo-Json | Set-Content $sdManifestPath
}
# The same order as a package's manifest.
$sorted = [ordered]@{}
$wanted.Keys | Sort-Object | ForEach-Object { $sorted[$_] = $wanted[$_] }
$sorted | ConvertTo-Json | Set-Content $sdManifestPath
$cias = @($wanted.Keys | Where-Object { $_ -like 'cias/*.cia' }) -join ', '
Write-Host "Restored ${Test}: install sdmc:/$cias"
