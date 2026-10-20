# Puts an archived test's whole SD environment back on the 3DS over Wi-Fi (chomp-cli), from
# out\test-archive\<test> (written by make-sdcard-openmw.ps1 for every package).
#   scripts\restore-test.ps1 -Test test77          # the files that differ from what the SD holds now
#   scripts\restore-test.ps1 -Test test77 -List    # only say what would change
# What the SD holds is out\sdcard-openmw.manifest.json (the last package, or the last restore): only the files
# that differ are sent, files the test did not have are deleted (under openmw/ and cias/ only), and the manifest
# then records the restored test, so the next package's update list is right. The tester then installs
# sdmc:/cias/openmw3ds.cia.
param(
    [Parameter(Mandatory)][string]$Test,
    [switch]$List
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
$chomp = Join-Path $root '.tools\chomp\chomp-cli.exe'
$sdManifestPath = Join-Path $out 'sdcard-openmw.manifest.json'

$wanted = @{}
(Get-Content "$archive\manifest.json" -Raw | ConvertFrom-Json).PSObject.Properties | ForEach-Object { $wanted[$_.Name] = $_.Value }
$onSd = @{}
if (Test-Path $sdManifestPath) {
    (Get-Content $sdManifestPath -Raw | ConvertFrom-Json).PSObject.Properties | ForEach-Object { $onSd[$_.Name] = $_.Value }
}
$send = @($wanted.Keys | Where-Object { $onSd[$_] -ne $wanted[$_] } | Sort-Object)
$delete = @($onSd.Keys | Where-Object { -not $wanted.ContainsKey($_) -and ($_ -like 'openmw/*' -or $_ -like 'cias/*') } | Sort-Object)

Get-Content "$archive\BUILD.txt" | ForEach-Object { Write-Host $_ }
Write-Host "Send ($($send.Count)):"; $send | ForEach-Object { Write-Host "  sdmc:/$_" }
if ($delete.Count) { Write-Host "Delete ($($delete.Count)):"; $delete | ForEach-Object { Write-Host "  sdmc:/$_" } }
if ($List) { return }

function Invoke-Chomp([string[]]$arguments) {
    & $chomp --json --quiet @arguments | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "chomp-cli $($arguments -join ' ') failed (exit $LASTEXITCODE)" }
}
foreach ($rel in $send) {
    $dir = Split-Path $rel -Parent
    Invoke-Chomp @('put', (Join-Path "$archive\sd" $rel.Replace('/', '\')), "sd:/$($dir.Replace('\', '/'))/")
    $onSd[$rel] = $wanted[$rel] # recorded per file: an interrupted restore can be run again
    $onSd | ConvertTo-Json | Set-Content $sdManifestPath
}
foreach ($rel in $delete) {
    Invoke-Chomp @('rm', "sd:/$rel")
    $onSd.Remove($rel)
    $onSd | ConvertTo-Json | Set-Content $sdManifestPath
}
# The same order as a package's manifest.
$sorted = [ordered]@{}
$wanted.Keys | Sort-Object | ForEach-Object { $sorted[$_] = $wanted[$_] }
$sorted | ConvertTo-Json | Set-Content $sdManifestPath
Write-Host "Restored ${Test}: install sdmc:/cias/openmw3ds.cia"
