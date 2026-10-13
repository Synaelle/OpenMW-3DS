# Build (optional), launch in Azahar, wait for the app to finish, collect artefacts.
#   scripts\run-azahar.ps1                               # interactive: just build + launch testapp.3dsx
#   scripts\run-azahar.ps1 -Autorun "probes bench"       # automated run, waits for completion
#   scripts\run-azahar.ps1 -Autorun "crash:data" -NoBuild
#   scripts\run-azahar.ps1 -App out\testapp.cxi          # .cxi = same exheader as the .cia (178 MB mode)
# Artefacts (log, crash.log, probe.txt, screenshots as PNG, Azahar log) land in out\runs\<timestamp>\.
param(
    [string]$App = 'out\testapp.3dsx',
    [string]$Autorun = '',
    [int]$TimeoutSec = 180,
    [switch]$NoBuild,
    [string]$Azahar = 'D:\Emulators\Azahar3DS\azahar.exe',
    [string]$Config = 'RelWithDebInfo'
)
$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $RepoRoot

if (-not $NoBuild) { & "$PSScriptRoot\build.ps1" -Config $Config | Out-Host }

$AppPath = Resolve-Path $App
$AzaharUser = Join-Path $env:APPDATA 'Azahar'
$SdOpenmw = Join-Path $AzaharUser 'sdmc\openmw'
New-Item -ItemType Directory -Force $SdOpenmw | Out-Null

# Clean previous run outputs (keep any game data the user placed there).
Get-ChildItem $SdOpenmw -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^(openmw\.log|crash\.log|probe\.txt|autorun\.(txt|done)|shot_.*\.bmp|perf.*\.csv)$' } |
    Remove-Item -Force
if ($Autorun) {
    # Emulator runs always capture screenshots (cheap there; opt-in on hardware).
    if ($Autorun -match 'bench' -and $Autorun -notmatch 'screens') { $Autorun += ' screens' }
    Set-Content -Path (Join-Path $SdOpenmw 'autorun.txt') -Value $Autorun -NoNewline
}

Get-Process azahar -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

if ($AppPath.Path.EndsWith('.cia')) {
    throw 'Azahar cannot boot a .cia from the command line (-i hangs without UI); use the .cxi twin instead'
}

$azLog = Join-Path $AzaharUser 'log\azahar_log.txt'
$proc = Start-Process -FilePath $Azahar -ArgumentList "`"$($AppPath.Path)`"" -PassThru
Write-Host "Azahar started (pid $($proc.Id)) with $($AppPath.Path)"

if (-not $Autorun) {
    Write-Host 'Interactive run: close Azahar when finished. Logs: ' $SdOpenmw
    return
}

# Captures every visible top-level window of a process (main window + dialogs such as Azahar's
# exception popup) to PNG files.
Add-Type -AssemblyName System.Drawing
if (-not ('WinCap' -as [type])) {
    Add-Type -TypeDefinition @"
using System; using System.Collections.Generic; using System.Runtime.InteropServices;
public static class WinCap {
    delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [StructLayout(LayoutKind.Sequential)] struct RECT { public int L, T, R, B; }
    // Returns (hwnd, width, height) for each visible top-level window of the process.
    public static List<long[]> Windows(int pid) {
        var list = new List<long[]>();
        EnumWindows((h, p) => {
            uint wp; GetWindowThreadProcessId(h, out wp);
            if (wp != pid || !IsWindowVisible(h)) return true;
            RECT r; GetWindowRect(h, out r);
            if (r.R - r.L >= 50 && r.B - r.T >= 50) list.Add(new long[] { h.ToInt64(), r.R - r.L, r.B - r.T });
            return true;
        }, IntPtr.Zero);
        return list;
    }
}
"@
}
function Save-ProcessWindows([int]$ProcId, [string]$Prefix) {
    $n = 0
    foreach ($w in [WinCap]::Windows($ProcId)) {
        $bmp = New-Object System.Drawing.Bitmap ([int]$w[1]), ([int]$w[2])
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $dc = $g.GetHdc(); [void][WinCap]::PrintWindow([IntPtr]$w[0], $dc, 2); $g.ReleaseHdc($dc); $g.Dispose()
        $path = "$Prefix$n.png"; $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
        Write-Host "window capture: $path"; $n++
    }
}

$deadline = (Get-Date).AddSeconds($TimeoutSec)
$result = 'timeout'
while ((Get-Date) -lt $deadline) {
    if (Test-Path (Join-Path $SdOpenmw 'autorun.done')) { $result = 'done'; break }
    if (Test-Path (Join-Path $SdOpenmw 'crash.log')) { Start-Sleep -Seconds 3; $result = 'crash'; break }
    if ($proc.HasExited) { $result = 'emulator-exited'; break }
    Start-Sleep -Milliseconds 500
}
Start-Sleep -Seconds 1
$runDir = Join-Path $RepoRoot ("out\runs\" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force $runDir | Out-Null
if (-not $proc.HasExited) {
    # Snapshot the emulator windows (catches emulator dialogs, e.g. its exception popup).
    Save-ProcessWindows $proc.Id (Join-Path $runDir 'window_')
    Stop-Process -Id $proc.Id -Force
}
Remove-Item (Join-Path $SdOpenmw 'autorun.txt') -ErrorAction SilentlyContinue

# Collect.
Get-ChildItem $SdOpenmw -File | Where-Object { $_.Name -match '\.(log|txt|csv|done)$' } | Copy-Item -Destination $runDir
if (Test-Path $azLog) { Copy-Item $azLog (Join-Path $runDir 'azahar_log.txt') }

Get-ChildItem $SdOpenmw -Filter 'shot_*.bmp' | ForEach-Object {
    $img = [System.Drawing.Image]::FromFile($_.FullName)
    $img.Save((Join-Path $runDir ($_.BaseName + '.png')), [System.Drawing.Imaging.ImageFormat]::Png)
    $img.Dispose()
}

Write-Host "Result: $result"
Write-Host "Artefacts: $runDir"
Get-ChildItem $runDir | Select-Object Name, Length | Format-Table -AutoSize | Out-Host
if ($result -eq 'crash') {
    $elf = [IO.Path]::ChangeExtension($AppPath.Path, '.elf')
    if (Test-Path $elf) { & "$PSScriptRoot\symbolize.ps1" -CrashLog (Join-Path $runDir 'crash.log') -Elf $elf }
}
