#!/bin/bash
# Builds the Linux AppImage of the setup app, in an Ubuntu 22.04 container (Docker): its glibc (2.35) is old enough
# for most distributions in use, and nothing is installed on the machine that builds it.
#
#     setup/linux/build-appimage.sh            # from Linux or WSL with Docker
#
# Writes setup/out/OpenMW-3DS-Setup-x86_64.AppImage (and the command-line tool, openmw3ds-setup-cli-x86_64). The
# release puts the "port" folder next to the AppImage: the app looks for it there.
set -euo pipefail

LINUXDEPLOY_TAG=1-alpha-20251107-1

if [ "${1:-}" != "--inside" ]; then
    setup=$(cd "$(dirname "$0")/.." && pwd)
    exec docker run --rm -v "$setup:/src" -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" ubuntu:22.04 \
        bash /src/linux/build-appimage.sh --inside
fi

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq build-essential cmake ninja-build pkg-config libsdl2-dev wget file ca-certificates \
    > /dev/null

cmake -S /src -B /tmp/build -G Ninja -DCMAKE_BUILD_TYPE=Release > /tmp/configure.log
cmake --build /tmp/build

# The AppDir: the program, its desktop entry and icon; linuxdeploy adds the libraries it needs (SDL2's) and packs it.
appdir=/tmp/AppDir
mkdir -p "$appdir/usr/bin"
cp /tmp/build/openmw3ds-setup "$appdir/usr/bin/"
cd /tmp
wget -q "https://github.com/linuxdeploy/linuxdeploy/releases/download/$LINUXDEPLOY_TAG/linuxdeploy-x86_64.AppImage"
chmod +x linuxdeploy-x86_64.AppImage
# No FUSE in a container: the tool runs extracted.
export APPIMAGE_EXTRACT_AND_RUN=1
OUTPUT=OpenMW-3DS-Setup-x86_64.AppImage ./linuxdeploy-x86_64.AppImage --appdir "$appdir" \
    --desktop-file /src/linux/openmw3ds-setup.desktop --icon-file /src/linux/openmw3ds-setup.png \
    --output appimage > /tmp/linuxdeploy.log

mkdir -p /src/out
cp OpenMW-3DS-Setup-x86_64.AppImage /src/out/
cp /tmp/build/openmw3ds-setup-cli /src/out/openmw3ds-setup-cli-x86_64
chown "${HOST_UID:-0}:${HOST_GID:-0}" /src/out/OpenMW-3DS-Setup-x86_64.AppImage /src/out/openmw3ds-setup-cli-x86_64
ls -la /src/out
