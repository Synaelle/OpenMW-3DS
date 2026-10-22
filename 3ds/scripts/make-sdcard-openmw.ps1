# Builds out\sdcard-openmw\: an OpenMW hardware test package to copy onto the root of the 3DS SD card.
#   scripts\make-sdcard-openmw.ps1 [-NoBuild] [-Start "Seyda Neen, Arrille's Tradehouse"] [-NoDebug]
# Morrowind's data files are NOT included: the tester copies Morrowind.esm and Morrowind.bsa from
# their own installation into sdmc:/openmw/data/ (see the README in the package).
param(
    [switch]$NoBuild,
    # Arrille's: the Census office's first door into it is the jail's PrisonMarker, whose destination is outside
    # the walls, so --start dropped the player into the void (OpenMW behaviour, see PORTING_NOTES).
    [string]$Start = "Seyda Neen, Arrille's Tradehouse",
    # Release build: no debug tools (see build-openmw.ps1).
    [switch]$NoDebug
)
$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
if (-not $NoBuild) { & "$PSScriptRoot\build-openmw.ps1" -NoDebug:$NoDebug | Out-Host }

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
    # (packaging/test-start.txt, the console's output goes to openmw.log).
    Copy-Item (Join-Path $RepoRoot 'packaging\test-start.txt') "$sd\openmw\test-start.txt"
    # The hardware test's runs, chosen on the bottom screen at startup (main3ds.cpp testMenu): it creates the
    # chosen run's switch files, so the tester never handles them.
    Copy-Item (Join-Path $RepoRoot 'packaging\test-menu.txt') "$sd\openmw\test-menu.txt"
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

# What the test is about: test-menu.txt's title line (debug packages).
$testTitle = ''
$menuFile = Join-Path $RepoRoot 'packaging\test-menu.txt'
if (-not $NoDebug -and (Test-Path $menuFile)) {
    $testTitle = (Get-Content $menuFile | Where-Object { $_ -match '^title\s+(.+)' } | Select-Object -First 1) -replace '^title\s+', ''
}

@"
OpenMW 3DS - package ($stamp)
=============================
Build: "build $shortBuild" on the bottom screen while the game starts (log: $buildId). If it shows another
build, the new CIA is not the one installed.
$testTitle

1. Copy the CONTENTS of this folder to the ROOT of your SD card, merging folders and overwriting files (the
   openmw folder and cias/openmw3ds.cia). Your Morrowind files in sdmc:/openmw/data/ are not in the package,
   so they stay. The biggest files: openmw/data/sound3ds.bsa (~240 MB) and openmw/cooked/textures.o3tp
   (~80 MB).
2. Once: copy Morrowind.esm (~79 MB) and Morrowind.bsa (~310 MB) from your Morrowind "Data Files" folder into
   sdmc:/openmw/data/ (nothing else: Sound and Music come packed in openmw/data/sound3ds.bsa). They are only
   read, never modified. Sound also needs the 3DS's DSP firmware, sdmc:/3ds/dspfirm.cdc: if you've played
   homebrew with sound it's there, otherwise run the DSP1 homebrew once.
3. Install cias/openmw3ds.cia with FBI, then launch "OpenMW3DS" from the HOME Menu.
4. Controls:
     Circle Pad   walk (push past halfway to run)      C-Stick   look
     ZR           attack / cast                         ZL        jump
     L (hold)     sneak                                 R         1st / 3rd person
     A            activate (talk, pick up, open doors)  B         Inventory tab, and closes any menu
     X            ready weapon                          Y         ready magic
     D-pad Up     journal                               D-pad Down  rest / wait
     START        game menu                             SELECT    console (system keyboard)
   The bottom screen: the HUD, and the Inventory, Magic and Stats tabs (touch a tab, touch it again or the
   house to go back). They don't pause the game.
   Screenshots: the start menu's Options -> "Screenshot: On (experimental)" adds a camera to the HUD; each
   touch saves both screens to sdmc:/openmw/screenshots/. Rosalina's screenshots work too.
   Panic: L + R + START + SELECT held for a second quits the game.
5. Send back the logs folder (sdmc:/openmw/logs/, the run's own folder for a Debug start) and your
   screenshots. If you have to force the power off, still send logs/openmw3ds.log.
"@ | Set-Content "$sd\README.txt"

Write-Host "Package: $sd"
Get-ChildItem $sd -Recurse -File | Measure-Object Length -Sum | ForEach-Object { "{0} files, {1:N1} MB" -f $_.Count, ($_.Sum / 1MB) }

# Incremental update: out\sdcard-openmw-update\ holds only the files that differ from the previous
# package (copying the ~380 small files of resources/ over FTP is slow), and UPDATE.txt lists them.
# The manifest of the previous package is out\sdcard-openmw.manifest.json. -Full skips the diff.
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

# Test archive (the tester, 19/10: "archive the whole environment for previous tests"): the whole SD
# environment of this package in out\test-archive\<test>\ (<test> from test-menu.txt's "logs" line), so an
# old test can be put back exactly with scripts\restore-test.ps1. Files are stored once by content in
# out\test-archive\.store and hard-linked: a test costs only the files it changed (the game data, ~230 MB,
# once). Packaging the same test again replaces its archive.
$testName = (Get-Content (Join-Path $sd 'openmw\test-menu.txt') | Where-Object { $_ -match '^logs\s+(\S+)' } |
    Select-Object -First 1) -replace '^logs\s+', ''
if ($testName) {
    $archiveRoot = Join-Path $out 'test-archive'
    $store = Join-Path $archiveRoot '.store'
    $archive = Join-Path $archiveRoot $testName
    New-Item -ItemType Directory -Force $store | Out-Null
    if (Test-Path $archive) { Remove-Item -Recurse -Force $archive }
    foreach ($rel in $manifest.Keys) {
        $stored = Join-Path $store $manifest[$rel]
        if (-not (Test-Path $stored)) { Copy-Item (Join-Path $sd $rel) $stored }
        $dst = Join-Path $archive ('sd\' + $rel.Replace('/', '\'))
        New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
        New-Item -ItemType HardLink -Path $dst -Target $stored | Out-Null
    }
    $manifest | ConvertTo-Json | Set-Content (Join-Path $archive 'manifest.json')
    @("Test: $testName", "Build: $buildId (bottom screen: build $shortBuild)", "Packaged: $stamp",
        "Program with symbols: $archived") | Set-Content (Join-Path $archive 'BUILD.txt')
    Write-Host "Archived: $archive"
}

# Old ELFs: stripped to their symbols or deleted (scripts\prune-elf-archive.ps1, the tester's local script, not
# in the repo).
$prune = Join-Path $PSScriptRoot 'prune-elf-archive.ps1'
if (Test-Path $prune) { & $prune | Out-Host }
