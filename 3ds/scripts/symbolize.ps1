# Resolves the code addresses in a crash report (openmw3ds-crash.log) to function/file:line using the matching .elf.
#   scripts\symbolize.ps1 -CrashLog out\runs\<run>\openmw3ds-crash.log -Elf out\testapp.elf
#   scripts\symbolize.ps1 -CrashLog openmw3ds-crash.log -Elf out\          # auto-picks the matching archived .elf
# Keep the .elf of every build you put on hardware: addresses only resolve against the exact build.
param(
    [Parameter(Mandatory)][string]$CrashLog,
    [Parameter(Mandatory)][string]$Elf,
    [string]$Msys2Root = 'C:\msys64'
)
$ErrorActionPreference = 'Stop'
$addr2line = "$Msys2Root\opt\devkitpro\devkitARM\bin\arm-none-eabi-addr2line.exe"
if (-not (Test-Path $addr2line)) { throw "addr2line not found at $addr2line" }

$text = Get-Content $CrashLog -Raw
if (Test-Path $Elf -PathType Container) {
    # Pick the .elf whose text end (__exidx_start) matches the "Image:" line of the report.
    if ($text -notmatch 'Image: 0x[0-9A-Fa-f]{8} - 0x([0-9A-Fa-f]{8})') { throw 'crash log has no Image line' }
    $want = $Matches[1].ToLower()
    $nm = "$Msys2Root\opt\devkitpro\devkitARM\bin\arm-none-eabi-nm.exe"
    $found = Get-ChildItem $Elf -Filter *.elf | Where-Object {
        (& $nm $_.FullName 2>$null | Select-String ' __exidx_start$').Line -match "^$want"
    } | Select-Object -First 1
    if (-not $found) { throw "no .elf in $Elf matches image end 0x$want" }
    $Elf = $found.FullName
}
$inTrace = $false
$addrs = New-Object System.Collections.Generic.List[string]
$pcs = New-Object System.Collections.Generic.List[string]
foreach ($line in ($text -split "`r?`n")) {
    if ($line -match '^(Backtrace|Stack scan)') { $inTrace = $true; continue }
    if ($line -match '^\s*$') { $inTrace = $false }
    if ($line -match '^\s+#0 pc\s+(0x[0-9A-Fa-f]{8})') { $pcs.Add($Matches[1]); $addrs.Add($Matches[1]); continue }
    if ($inTrace) { foreach ($m in [regex]::Matches($line, '0x[0-9A-Fa-f]{8}')) { $addrs.Add($m.Value) } }
}
$unique = $addrs | Select-Object -Unique
if (-not $unique) { Write-Host 'No code addresses found.'; return }

Write-Host "Symbolized ($Elf):"
foreach ($a in $unique) {
    # Return addresses point after the call; step back one instruction for the call site.
    $v = [Convert]::ToUInt32($a.Substring(2), 16)
    $q = if ($pcs.Contains($a) -or $v -lt 4) { $a } else { '0x{0:X8}' -f ($v - 4) }
    $res = & $addr2line -e $Elf -f -C -i -p $q 2>$null
    "{0}  {1}" -f $a, (($res -join ' / ') -replace '\s+', ' ')
}
