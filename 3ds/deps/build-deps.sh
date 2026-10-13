#!/usr/bin/env bash
# Cross-builds OpenMW's third-party dependencies for the 3DS into $PREFIX.
# Run through scripts/build-deps.ps1 (MSYS2). Usage: build-deps.sh [component...]
# With no arguments, builds every component not yet built (stamp files in $PREFIX/.stamps).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEPS="$ROOT/deps"
WORK="$ROOT/.deps"
SRC="$WORK/src"
BUILD="$WORK/build"
PREFIX="$WORK/3ds"
DL="$WORK/downloads"
STAMPS="$PREFIX/.stamps"
JOBS="${JOBS:-$(nproc)}"
export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
TOOLCHAIN="$DEVKITPRO/cmake/3DS.cmake"
mkdir -p "$SRC" "$BUILD" "$PREFIX" "$DL" "$STAMPS"

log() { printf '\n=== %s\n' "$*"; }

# fetch <url> <file> : download once, verify against deps/hashes.txt (recorded on first download).
fetch() {
    local url="$1" file="$DL/$2"
    if [ ! -f "$file" ]; then
        curl -sSfL --retry 3 -o "$file.part" "$url"
        mv "$file.part" "$file"
    fi
    local sum; sum=$(sha256sum "$file" | cut -d' ' -f1)
    local want; want=$(grep -E "^[0-9a-f]{64}  $2\$" "$DEPS/hashes.txt" 2>/dev/null | cut -d' ' -f1 || true)
    if [ -z "$want" ]; then
        echo "$sum  $2" >> "$DEPS/hashes.txt"
        echo "recorded new hash for $2 (commit deps/hashes.txt)"
    elif [ "$want" != "$sum" ]; then
        echo "HASH MISMATCH for $2: got $sum want $want" >&2
        exit 1
    fi
}

# unpack <archive> <dirname> : extract into $SRC/<dirname> (fresh), apply deps/patches/<dirname>/*.patch
unpack() {
    local archive="$DL/$1" name="$2"
    rm -rf "$SRC/$name" "$SRC/.unpack"
    mkdir -p "$SRC/.unpack"
    case "$archive" in
        *.zip) unzip -q "$archive" -d "$SRC/.unpack" ;;
        *) tar -xf "$archive" -C "$SRC/.unpack" ;;
    esac
    mv "$SRC/.unpack"/* "$SRC/$name"
    rmdir "$SRC/.unpack"
    # Pristine copy, used by deps/mkpatch.sh to generate new patches.
    rm -rf "$WORK/pristine/$name"
    mkdir -p "$WORK/pristine"
    cp -r "$SRC/$name" "$WORK/pristine/$name"
    if [ -d "$DEPS/patches/$name" ]; then
        for p in "$DEPS/patches/$name"/*.patch; do
            [ -e "$p" ] || continue
            echo "patch: $(basename "$p")"
            patch -d "$SRC/$name" -p1 --forward --quiet < "$p"
        done
    fi
}

# cmake_build <srcdir> <builddir-name> [cmake args...]
cmake_build() {
    local src="$1" name="$2"; shift 2
    rm -rf "$BUILD/$name"
    cmake -S "$src" -B "$BUILD/$name" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DCMAKE_PREFIX_PATH="$PREFIX" \
        -DCMAKE_FIND_ROOT_PATH="$PREFIX;$DEVKITPRO/portlibs/3ds;$DEVKITPRO/libctru" \
        -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        "$@"
    cmake --build "$BUILD/$name" -j "$JOBS"
    cmake --install "$BUILD/$name"
}

# ---------------------------------------------------------------- components

build_sdl2() {
    local v=2.32.10
    fetch "https://github.com/libsdl-org/SDL/releases/download/release-$v/SDL2-$v.tar.gz" "SDL2-$v.tar.gz"
    unpack "SDL2-$v.tar.gz" sdl2
    # Events, joystick, touch and timers only: OpenMW's window/GL context is replaced on 3DS.
    cmake_build "$SRC/sdl2" sdl2 -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF -DSDL2_DISABLE_SDL2MAIN=ON
}

build_boost() {
    local v=1.92.0
    fetch "https://github.com/boostorg/boost/releases/download/boost-$v/boost-$v-cmake.tar.xz" "boost-$v-cmake.tar.xz"
    unpack "boost-$v-cmake.tar.xz" boost
    # Boost.Config has no __3DS__ platform file, so it never enables pthreads (and falls back to Win32
    # code paths); Boost.Container's bundled dlmalloc must not use mmap. Env flags are appended to
    # the toolchain's own CMAKE_<LANG>_FLAGS_INIT rather than replacing them.
    CFLAGS="-DBOOST_HAS_PTHREADS -DHAVE_MMAP=0" CXXFLAGS="-DBOOST_HAS_PTHREADS -DHAVE_MMAP=0" \
    cmake_build "$SRC/boost" boost \
        -DBOOST_INCLUDE_LIBRARIES="program_options" \
        -DBUILD_TESTING=OFF
    # Only program_options needs compiling. Everything else OpenMW uses (geometry rtree, iostreams
    # stream/tee devices) is header-only, so install all headers instead of building those libraries
    # (geometry drags in Boost.Thread and iostreams needs mmap: neither builds on newlib).
    for inc in "$SRC"/boost/libs/*/include "$SRC"/boost/libs/numeric/*/include; do
        [ -d "$inc/boost" ] && cp -rn "$inc/boost" "$PREFIX/include/"
    done
}

build_bullet() {
    local v=3.25
    fetch "https://github.com/bulletphysics/bullet3/archive/refs/tags/$v.tar.gz" "bullet3-$v.tar.gz"
    unpack "bullet3-$v.tar.gz" bullet
    # Same configuration as openmw/extern/CMakeLists.txt (OpenMW requires double precision).
    cmake_build "$SRC/bullet" bullet \
        -DUSE_DOUBLE_PRECISION=ON -DBULLET2_MULTITHREADING=ON \
        -DBUILD_UNIT_TESTS=OFF -DBUILD_BULLET2_DEMOS=OFF -DBUILD_CPU_DEMOS=OFF -DBUILD_OPENGL3_DEMOS=OFF \
        -DBUILD_EXTRAS=OFF -DBUILD_CLSOCKET=OFF -DBUILD_ENET=OFF -DBUILD_EGL=OFF \
        -DBUILD_PYBULLET=OFF -DUSE_GRAPHICAL_BENCHMARK=OFF -DINSTALL_LIBS=ON \
        -DBUILD_BULLET3=OFF  # OpenCL/GPU pipeline (needs dlopen); OpenMW uses BulletCollision + LinearMath only
}

# POSIX bits newlib/libctru lack (dlfcn.h, ...): header-only shims in deps/compat/include.
build_compat() {
    mkdir -p "$PREFIX/include"
    cp -r "$DEPS/compat/include/." "$PREFIX/include/"
}

# Stub OpenGL: Mesa/Khronos headers + libGL.a of no-op functions (see deps/stubgl/stubgl_special.c).
build_stubgl() {
    local mesa=mesa-24.0.0
    fetch "https://gitlab.freedesktop.org/mesa/mesa/-/raw/$mesa/include/GL/gl.h" "gl-$mesa.h"
    fetch "https://gitlab.freedesktop.org/mesa/mesa/-/raw/$mesa/include/GL/glext.h" "glext-$mesa.h"
    fetch "https://gitlab.freedesktop.org/mesa/mesa/-/raw/$mesa/include/KHR/khrplatform.h" "khrplatform-$mesa.h"
    mkdir -p "$PREFIX/include/GL" "$PREFIX/include/KHR"
    cp "$DL/gl-$mesa.h" "$PREFIX/include/GL/gl.h"
    cp "$DL/glext-$mesa.h" "$PREFIX/include/GL/glext.h"
    cp "$DL/khrplatform-$mesa.h" "$PREFIX/include/KHR/khrplatform.h"
    stubgl_generate
}

# Rebuilds $PREFIX/lib/libGL.a: hand-written stubs + a no-op for every other gl* symbol that the
# installed static libraries reference. Re-run after building anything that calls GL (osg, mygui).
stubgl_generate() {
    local dir="$BUILD/stubgl" cc="$DEVKITARM/bin/arm-none-eabi-gcc" nm="$DEVKITARM/bin/arm-none-eabi-nm"
    mkdir -p "$dir"
    local flags="-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -O2 -ffunction-sections -I$PREFIX/include"
    "$cc" $flags -c "$DEPS/stubgl/stubgl_special.c" -o "$dir/special.o"
    "$nm" -g --defined-only "$dir/special.o" | awk '{print $3}' | sort -u > "$dir/defined.txt"
    : > "$dir/wanted.txt"
    for lib in "$PREFIX"/lib/*.a; do
        [ "$(basename "$lib")" = libGL.a ] && continue
        "$nm" -u "$lib" 2>/dev/null | awk '{print $2}' | grep -E '^gl[A-Z]' >> "$dir/wanted.txt" || true
    done
    sort -u "$dir/wanted.txt" | comm -23 - "$dir/defined.txt" > "$dir/generated.txt"
    {
        echo "/* generated by deps/build-deps.sh: no-op GL entry points */"
        while read -r sym; do echo "int $sym(void) { return 0; }"; done < "$dir/generated.txt"
    } > "$dir/generated.c"
    "$cc" $flags -fno-builtin -c "$dir/generated.c" -o "$dir/generated.o"
    rm -f "$PREFIX/lib/libGL.a"
    "$DEVKITARM/bin/arm-none-eabi-ar" rcs "$PREFIX/lib/libGL.a" "$dir/special.o" "$dir/generated.o"
    echo "stub libGL.a: $(wc -l < "$dir/defined.txt") hand-written + $(wc -l < "$dir/generated.txt") generated no-ops"
}

build_osg() {
    local rev=9b384d5439977784e5cd250ff8ff7966928ddd27   # same commit as openmw/extern/CMakeLists.txt
    fetch "https://github.com/OpenMW/osg/archive/$rev.zip" "osg-$rev.zip"
    unpack "osg-$rev.zip" osg
    # Mirrors openmw/extern/CMakeLists.txt, minus the dae/ktx plugins; GL comes from the stub.
    cmake_build "$SRC/osg" osg \
        -DOPENGL_PROFILE=GL2 \
        -DOPENGL_INCLUDE_DIR="$PREFIX/include" -DOPENGL_gl_LIBRARY="$PREFIX/lib/libGL.a" \
        -DOSG_WINDOWING_SYSTEM=None \
        -DDYNAMIC_OPENSCENEGRAPH=OFF -DDYNAMIC_OPENTHREADS=OFF \
        -DOSG_USE_FLOAT_MATRIX=OFF -DOSG_USE_FLOAT_PLANE=OFF -DOSG_USE_FLOAT_QUAT=OFF \
        -DBUILD_OSG_APPLICATIONS=OFF -DBUILD_OSG_EXAMPLES=OFF -DBUILD_OSG_DEPRECATED_SERIALIZERS=OFF \
        -DBUILD_OSG_PLUGINS_BY_DEFAULT=OFF \
        -DBUILD_OSG_PLUGIN_BMP=ON -DBUILD_OSG_PLUGIN_DDS=ON -DBUILD_OSG_PLUGIN_FREETYPE=ON \
        -DBUILD_OSG_PLUGIN_JPEG=ON -DBUILD_OSG_PLUGIN_OSG=ON -DBUILD_OSG_PLUGIN_PNG=ON -DBUILD_OSG_PLUGIN_TGA=ON \
        -D_OPENTHREADS_ATOMIC_USE_GCC_BUILTINS_EXITCODE=0 \
        -D_OPENTHREADS_ATOMIC_USE_GCC_BUILTINS_EXITCODE__TRYRUN_OUTPUT=""
    stubgl_generate
}

ALL_COMPONENTS=(compat sdl2 boost bullet stubgl osg)

# ---------------------------------------------------------------- driver
components=("$@")
[ ${#components[@]} -eq 0 ] && components=("${ALL_COMPONENTS[@]}")
for c in "${components[@]}"; do
    if [ $# -eq 0 ] && [ -f "$STAMPS/$c" ]; then
        echo "skip $c (already built; pass it explicitly to rebuild)"
        continue
    fi
    log "building $c"
    "build_$c"
    date > "$STAMPS/$c"
done
log "done: prefix $PREFIX"
