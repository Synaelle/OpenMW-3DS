# Reproducible host setup for the OpenMW 3DS port (Windows).
# Installs: MSYS2 (C:\msys64) + devkitPro pacman repos + 3DS toolchain/portlibs + host build tools,
# and fetches makerom/bannertool into .tools\bin. Safe to re-run (idempotent).
param(
    [string]$Msys2Root = 'C:\msys64'
)
$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
$ToolsBin = Join-Path $RepoRoot '.tools\bin'
$Dl = Join-Path $RepoRoot '.tools\downloads'
New-Item -ItemType Directory -Force $ToolsBin, $Dl | Out-Null

# devkitPro's package server rejects non-pacman user agents (Cloudflare).
$PacmanUA = 'pacman/6.1.0 (MSYS_NT-10.0 x86_64) libalpm/15.0.0'
# WinterMute's package signing key (verified against keyserver + devkitpro-keyring package).
$DkpKey = 'BC26F752D25B92CE272E0F44F7FD5492264BB9D0'

function Invoke-Msys([string]$cmd) {
    $env:MSYSTEM = 'MSYS'; $env:CHERE_INVOKING = '1'
    & "$Msys2Root\usr\bin\bash.exe" -lc $cmd
    if ($LASTEXITCODE -ne 0) { throw "msys command failed: $cmd" }
}

# 1. MSYS2
if (-not (Test-Path "$Msys2Root\usr\bin\bash.exe")) {
    $rel = (Invoke-RestMethod 'https://api.github.com/repos/msys2/msys2-installer/releases?per_page=5') |
        Where-Object { $_.tag_name -ne 'nightly-x86_64' } | Select-Object -First 1
    $asset = $rel.assets | Where-Object name -match '^msys2-x86_64-\d+\.exe$'
    $exe = Join-Path $Dl $asset.name
    Invoke-WebRequest $asset.browser_download_url -OutFile $exe
    if ((Get-AuthenticodeSignature $exe).Status -ne 'Valid') { throw 'MSYS2 installer signature invalid' }
    & $exe install --root $Msys2Root --confirm-command --accept-messages | Out-Null
    Invoke-Msys 'pacman --noconfirm -Syuu' ; Invoke-Msys 'pacman --noconfirm -Syuu'
}

# 2. devkitPro pacman repos
Invoke-Msys @"
set -e
if ! grep -q '^\[dkp-libs\]' /etc/pacman.conf; then
  pacman-key --recv-keys $DkpKey --keyserver keyserver.ubuntu.com
  pacman-key --lsign-key $DkpKey
  mkdir -p /tmp/dkp && cd /tmp/dkp
  curl -sSf -A '$PacmanUA' -o devkitpro-keyring.pkg.tar.zst https://pkg.devkitpro.org/devkitpro-keyring.pkg.tar.zst
  pacman --noconfirm -U devkitpro-keyring.pkg.tar.zst
  printf '\n[dkp-libs]\nServer = https://pkg.devkitpro.org/packages\n\n[dkp-windows]\nServer = https://pkg.devkitpro.org/packages/windows/\$arch/\n' >> /etc/pacman.conf
fi
"@

# 3. Packages
Invoke-Msys 'pacman --noconfirm --needed -Sy 3ds-dev 3ds-zlib 3ds-lz4 3ds-yaml_cpp 3ds-liblua51 3ds-mpg123 3ds-libpng 3ds-freetype 3ds-libjpeg-turbo 3ds-bzip2 make cmake ninja git patch python unzip zip autoconf automake libtool pkgconf mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja'

# 4. makerom + bannertool (for .cia)
function Get-GhAsset([string]$repo, [string]$tag, [string]$pattern) {
    $r = Invoke-RestMethod "https://api.github.com/repos/$repo/releases/tags/$tag"
    $a = $r.assets | Where-Object name -match $pattern | Select-Object -First 1
    $zip = Join-Path $Dl $a.name
    if (-not (Test-Path $zip)) { Invoke-WebRequest $a.browser_download_url -OutFile $zip }
    $out = Join-Path $Dl ([IO.Path]::GetFileNameWithoutExtension($a.name))
    Expand-Archive $zip -DestinationPath $out -Force
    return $out
}
if (-not (Test-Path "$ToolsBin\makerom.exe")) {
    $d = Get-GhAsset '3DSGuy/Project_CTR' 'makerom-v0.19.0' 'win_x86_64\.zip$'
    Get-ChildItem $d -Recurse -Filter makerom.exe | Select-Object -First 1 | Copy-Item -Destination $ToolsBin
}
if (-not (Test-Path "$ToolsBin\bannertool.exe")) {
    $d = Get-GhAsset 'carstene1ns/3ds-bannertool' '1.2.3' 'windows\.zip$'
    Get-ChildItem $d -Recurse -Filter bannertool.exe | Where-Object FullName -match 'x86_64|win64|64' | Select-Object -First 1 | Copy-Item -Destination $ToolsBin
    if (-not (Test-Path "$ToolsBin\bannertool.exe")) {
        Get-ChildItem $d -Recurse -Filter bannertool.exe | Select-Object -First 1 | Copy-Item -Destination $ToolsBin
    }
}
# 5. Python packages for tools/cook_textures.py and packaging/make_assets.py.
python -m pip install --user --quiet pillow numpy

Write-Host "Bootstrap complete. Toolchain: $Msys2Root\opt\devkitpro ; extra tools: $ToolsBin"
