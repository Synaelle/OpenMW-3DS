# Keeps out\elf-archive small (each ELF with its debug info is ~1.5 GB; 79 of them were 113 GB, the tester, 20/10):
#   - the newest $KeepFull ELFs stay whole (file and line numbers for symbolize.ps1);
#   - older ELFs that an archived test points to (out\test-archive\<test>\BUILD.txt, "Program with symbols") are
#     stripped of their debug info (~73 MB: crash backtraces still get function names, not lines);
#   - the other older ELFs (packages no archived test uses) are deleted.
# Whole ELFs are also kept on the tester's Google Drive ($Remote, rclone): a new ELF is uploaded in the background
# after its package, never overwriting what is there (the stripped local copy must not replace the whole one), and
# an ELF is only stripped once the Drive holds its whole size (uploaded first, waiting, if not). Without rclone or
# with -Remote '' nothing goes to the Drive and old ELFs are still stripped.
# Run by make-sdcard-openmw.ps1 after each package; safe to run again.
param(
    [int]$KeepFull = 3,
    [string]$Remote = 'gdrive:OpenMW 3DS/elf-archive',
    [string]$Msys2Root = 'C:\msys64'
)
$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
$archive = Join-Path $RepoRoot 'out\elf-archive'
$tests = Join-Path $RepoRoot 'out\test-archive'
$objcopy = "$Msys2Root\opt\devkitpro\devkitARM\bin\arm-none-eabi-objcopy.exe"
if (-not (Test-Path $archive)) { return }

$referenced = @{}
if (Test-Path $tests) {
    foreach ($build in Get-ChildItem $tests -Filter BUILD.txt -Recurse) {
        foreach ($line in Get-Content $build.FullName) {
            if ($line -match 'Program with symbols:\s*(.+\.elf)\s*$') { $referenced[[IO.Path]::GetFileName($Matches[1])] = $true }
        }
    }
}

$rclone = if ($Remote) { (Get-Command rclone -ErrorAction SilentlyContinue).Source } else { $null }
# The tester's own upload options (their Explorer "Upload to Google Drive", Upload-ToGDrive.ps1): big chunks for
# big files.
# -q: no notices on stderr (Windows PowerShell's Stop turns any stderr line from a program into an error).
$rcloneOptions = @('-q', '--local-encoding', 'None', '--drive-chunk-size', '256M', '--drive-disable-http2=false')
$onDrive = @{} # name -> size on the Drive
$listed = $false
if ($rclone) {
    $ErrorActionPreference = 'Continue'
    $listing = & $rclone lsjson $Remote --files-only -q 2>$null
    $ErrorActionPreference = 'Stop'
    if ($LASTEXITCODE -eq 0) {
        $listed = $true
        if ($listing) {
            $files = $listing | Out-String | ConvertFrom-Json # an array (Windows PowerShell: one object holding it)
            foreach ($f in $files) { $onDrive[$f.Name] = [int64]$f.Size }
        }
    }
    else { Write-Host "ELF archive: the Drive ($Remote) can't be listed; nothing is stripped this time" }
}

$elfs = @(Get-ChildItem $archive -Filter 'openmw-*.elf' | Sort-Object Name -Descending)
$before = ($elfs | Measure-Object Length -Sum).Sum
$stripped = 0; $deleted = 0
for ($i = $KeepFull; $i -lt $elfs.Count; ++$i) {
    $elf = $elfs[$i]
    if (-not $referenced.ContainsKey($elf.Name)) {
        Remove-Item -Force $elf.FullName
        ++$deleted
        continue
    }
    if ($elf.Length -lt 300MB) { continue } # stripped already
    if ($rclone) {
        if (-not $listed) { continue } # the Drive couldn't be checked: keep its debug info
        if (-not $onDrive.ContainsKey($elf.Name)) {
            Write-Host "ELF archive: uploading $($elf.Name) whole before stripping it"
            $ErrorActionPreference = 'Continue'
            & $rclone copyto $elf.FullName "$Remote/$($elf.Name)" --ignore-existing @rcloneOptions
            $ErrorActionPreference = 'Stop'
            if ($LASTEXITCODE -eq 0) { $onDrive[$elf.Name] = $elf.Length }
        }
        if ($onDrive[$elf.Name] -ne $elf.Length) {
            Write-Host "ELF archive: the Drive has no whole copy of $($elf.Name): kept whole here"
            continue
        }
    }
    $tmp = "$($elf.FullName).stripping"
    & $objcopy --strip-debug $elf.FullName $tmp
    if ($LASTEXITCODE -ne 0) { Remove-Item -Force -ErrorAction SilentlyContinue $tmp; throw "objcopy failed on $($elf.Name)" }
    Move-Item -Force $tmp $elf.FullName
    ++$stripped
}
$after = (Get-ChildItem $archive -Filter 'openmw-*.elf' | Measure-Object Length -Sum).Sum
# The newest ELFs to the Drive, in the background (the package doesn't wait; ignore-existing never replaces).
if ($rclone) {
    $copyArgs = @('copy', "`"$archive`"", "`"$Remote`"", '--include', 'openmw-*.elf', '--ignore-existing',
        '--max-size', '3G', '--min-size', '300M') + $rcloneOptions
    Start-Process -WindowStyle Hidden -FilePath $rclone -ArgumentList $copyArgs
}
'ELF archive: {0} kept whole, {1} stripped, {2} deleted; {3:N1} GB -> {4:N1} GB' -f [Math]::Min($KeepFull, $elfs.Count), $stripped, $deleted, ($before / 1GB), ($after / 1GB)
