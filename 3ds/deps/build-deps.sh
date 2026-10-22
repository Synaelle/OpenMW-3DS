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
# Force-included in every TU we compile: makes int32_t `int` (see the header for why).
INT32_FLAG="-include $DEPS/compat/include/openmw3ds_int32.h"

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
# PCH is disabled: CMake writes MSYS-style paths (/c/...) into generated PCH headers, which the native
# Windows cross-compiler cannot open.
cmake_build() {
    local src="$1" name="$2"; shift 2
    rm -rf "$BUILD/$name"
    # int32_t = int ABI for everything we build (see deps/compat/include/openmw3ds_int32.h).
    # Env flags are appended to the toolchain's CMAKE_<LANG>_FLAGS_INIT, not replacing them.
    CFLAGS="$INT32_FLAG ${CFLAGS:-}" CXXFLAGS="$INT32_FLAG ${CXXFLAGS:-}" \
    cmake -S "$src" -B "$BUILD/$name" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DCMAKE_PREFIX_PATH="$PREFIX" \
        -DCMAKE_FIND_ROOT_PATH="$PREFIX;$DEVKITPRO/portlibs/3ds;$DEVKITPRO/libctru" \
        -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON \
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
    # code paths). Boost.Container's bundled dlmalloc must not use mmap. Env flags are appended to
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

# Bullet in single precision, in its own prefix $WORK/3ds-bullet-float (much cheaper collision sweeps on the ARM11
# than upstream's double precision, cmake/openmw-3ds-cache.cmake). The double-precision build went in M20.
build_bulletfloat() {
    local v=3.25
    fetch "https://github.com/bulletphysics/bullet3/archive/refs/tags/$v.tar.gz" "bullet3-$v.tar.gz"
    unpack "bullet3-$v.tar.gz" bullet
    # Same configuration as openmw/extern/CMakeLists.txt, precision aside.
    cmake_build "$SRC/bullet" bullet-float -DCMAKE_INSTALL_PREFIX="$WORK/3ds-bullet-float" \
        -DUSE_DOUBLE_PRECISION=OFF -DBULLET2_MULTITHREADING=ON \
        -DBUILD_UNIT_TESTS=OFF -DBUILD_BULLET2_DEMOS=OFF -DBUILD_CPU_DEMOS=OFF -DBUILD_OPENGL3_DEMOS=OFF \
        -DBUILD_EXTRAS=OFF -DBUILD_CLSOCKET=OFF -DBUILD_ENET=OFF -DBUILD_EGL=OFF \
        -DBUILD_PYBULLET=OFF -DUSE_GRAPHICAL_BENCHMARK=OFF -DINSTALL_LIBS=ON \
        -DBUILD_BULLET3=OFF  # OpenCL/GPU pipeline (needs dlopen). OpenMW uses BulletCollision + LinearMath only
}

# POSIX bits newlib/libctru lack (dlfcn.h, ...): header-only shims in deps/compat/include.
build_compat() {
    mkdir -p "$PREFIX/include" "$PREFIX/lib" "$BUILD/compat"
    cp -r "$DEPS/compat/include/." "$PREFIX/include/"
    # libcompat3ds.a: POSIX stubs (fchown, geteuid, ...) referenced by dependencies.
    local flags="-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -O2 -ffunction-sections -D__3DS__ $INT32_FLAG"
    local objs=() csrc o
    for csrc in "$DEPS"/compat/src/*.c; do
        o="$BUILD/compat/$(basename "$csrc" .c).o"
        "$DEVKITARM/bin/arm-none-eabi-gcc" $flags -I"$DEVKITPRO/libctru/include" -c "$csrc" -o "$o"
        objs+=("$o")
    done
    rm -f "$PREFIX/lib/libcompat3ds.a"
    "$DEVKITARM/bin/arm-none-eabi-ar" rcs "$PREFIX/lib/libcompat3ds.a" "${objs[@]}"
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
    local flags="-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -O2 -ffunction-sections -I$PREFIX/include $INT32_FLAG"
    "$cc" $flags -c "$DEPS/stubgl/stubgl_special.c" -o "$dir/special.o"
    "$nm" -g --defined-only "$dir/special.o" | awk '{print $3}' | sort -u > "$dir/defined.txt"
    : > "$dir/wanted.txt"
    for lib in "$PREFIX"/lib/*.a; do
        [ "$(basename "$lib")" = libGL.a ] && continue
        "$nm" -u "$lib" 2>/dev/null | awk '{print $2}' | grep -E '^gl[A-Z]' >> "$dir/wanted.txt" || true
    done
    grep -E "^gl[A-Z]" "$DEPS/stubgl/extra-symbols.txt" >> "$dir/wanted.txt" || true
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
    # Mirrors openmw/extern/CMakeLists.txt, minus the dae/ktx plugins. GL comes from the stub.
    # Unlike upstream, matrices/planes/quaternions are float (as in the Vita port): animation keyframes
    # store osg::Quat values, and with doubles Morrowind's base animations alone exhaust the heap.
    # The ARM11 VFP is also slower at double precision.
    cmake_build "$SRC/osg" osg \
        -DOPENGL_PROFILE=GL2 \
        -DOPENGL_INCLUDE_DIR="$PREFIX/include" -DOPENGL_gl_LIBRARY="$PREFIX/lib/libGL.a" \
        -DOSG_WINDOWING_SYSTEM=None \
        -DDYNAMIC_OPENSCENEGRAPH=OFF -DDYNAMIC_OPENTHREADS=OFF \
        -DOSG_USE_FLOAT_MATRIX=ON -DOSG_USE_FLOAT_PLANE=ON -DOSG_USE_FLOAT_QUAT=ON \
        -DBUILD_OSG_APPLICATIONS=OFF -DBUILD_OSG_EXAMPLES=OFF -DBUILD_OSG_DEPRECATED_SERIALIZERS=OFF \
        -DBUILD_OSG_PLUGINS_BY_DEFAULT=OFF \
        -DBUILD_OSG_PLUGIN_BMP=ON -DBUILD_OSG_PLUGIN_DDS=ON -DBUILD_OSG_PLUGIN_FREETYPE=ON \
        -DBUILD_OSG_PLUGIN_JPEG=ON -DBUILD_OSG_PLUGIN_OSG=ON -DBUILD_OSG_PLUGIN_PNG=ON -DBUILD_OSG_PLUGIN_TGA=ON \
        -D_OPENTHREADS_ATOMIC_USE_GCC_BUILTINS_EXITCODE=0 \
        -D_OPENTHREADS_ATOMIC_USE_GCC_BUILTINS_EXITCODE__TRYRUN_OUTPUT=""
    stubgl_generate
}

build_mygui() {
    local v=3.4.3
    fetch "https://github.com/MyGUI/mygui/archive/refs/tags/MyGUI$v.zip" "MyGUI$v.zip"
    unpack "MyGUI$v.zip" mygui
    # As openmw/extern/CMakeLists.txt, but the Dummy render system: OpenMW renders MyGUI itself
    # (components/myguiplatform), and on 3DS through the citro3d backend - no GL platform needed.
    cmake_build "$SRC/mygui" mygui \
        -DMYGUI_RENDERSYSTEM=1 -DMYGUI_DISABLE_PLUGINS=TRUE -DMYGUI_BUILD_DEMOS=OFF \
        -DMYGUI_BUILD_PLUGINS=OFF -DMYGUI_BUILD_TOOLS=OFF -DMYGUI_DONT_USE_OBSOLETE=ON \
        -DMYGUI_STATIC=ON -DMYGUI_USE_FREETYPE=ON
}

build_recast() {
    local rev=03259f3287ff8330f0d66fcd98d022edddffaa97   # same commit as openmw/extern/CMakeLists.txt
    fetch "https://github.com/OpenMW/recastnavigation/archive/$rev.zip" "recastnavigation-$rev.zip"
    unpack "recastnavigation-$rev.zip" recast
    cmake_build "$SRC/recast" recast \
        -DRECASTNAVIGATION_DEMO=OFF -DRECASTNAVIGATION_TESTS=OFF -DRECASTNAVIGATION_EXAMPLES=OFF
}

build_sqlite3() {
    fetch "https://www.sqlite.org/2023/sqlite-amalgamation-3410100.zip" "sqlite-amalgamation-3410100.zip"
    unpack "sqlite-amalgamation-3410100.zip" sqlite3
    cp "$DEPS/sqlite3/CMakeLists.txt" "$SRC/sqlite3/"
    cmake_build "$SRC/sqlite3" sqlite3
}

# Built here rather than taken from portlibs: it is C++, so it must use our int32_t = int ABI.
build_yamlcpp() {
    local rev=c2680200486572baf8221ba052ef50b58ecd816e   # same commit as openmw/extern/CMakeLists.txt
    fetch "https://github.com/jbeder/yaml-cpp/archive/$rev.zip" "yaml-cpp-$rev.zip"
    unpack "yaml-cpp-$rev.zip" yamlcpp
    cmake_build "$SRC/yamlcpp" yamlcpp -DYAML_CPP_BUILD_TESTS=OFF -DYAML_CPP_BUILD_TOOLS=OFF \
        -DYAML_CPP_BUILD_CONTRIB=OFF -DYAML_BUILD_SHARED_LIBS=OFF
}

build_icu() {
    # 73.2, not upstream's pinned 70.1: ICU 70 assumes int32_t == int, but arm-none-eabi's int32_t
    # is long (dozens of overload/override errors). 73.2 builds cleanly (the Vita port uses it too).
    # OpenMW only uses the stable MessageFormat/Locale API.
    local v=73-2 vu=73_2
    fetch "https://github.com/unicode-org/icu/releases/download/release-$v/icu4c-$vu-src.tgz" "icu4c-$vu-src.tgz"
    unpack "icu4c-$vu-src.tgz" icu
    local src="$SRC/icu/source"
    # The source tarball ships prebuilt data, which ignores ICU_DATA_FILTER_FILE: replace it with
    # the data sources so the filter actually applies.
    fetch "https://github.com/unicode-org/icu/releases/download/release-$v/icu4c-$vu-data.zip" "icu4c-$vu-data.zip"
    rm -rf "$src/data"
    unzip -q "$DL/icu4c-$vu-data.zip" -d "$src"
    local host="$BUILD/icu-host" cross="$BUILD/icu-3ds"

    # 1. Native host build (MSYS2 UCRT64 MinGW): ICU runs its own tools to build its data.
    rm -rf "$host"; mkdir -p "$host"
    # ICU 70 defaults to -std=c++11 -pedantic, which GCC 16's <limits> rejects: use gnu++17.
    env MSYSTEM=UCRT64 CHERE_INVOKING=1 CFLAGS="-O2" CXXFLAGS="-O2 -std=gnu++17" \
        bash -lc "cd '$host' && '$src/runConfigureICU' MinGW \
        --disable-tests --disable-samples --disable-extras --disable-icuio \
        && make -j$JOBS"
    # (Host build keeps ICU's default shared libs: static-only MinGW builds name libs libsicu*.a but
    # link the tools with -licu*, which fails. Only the host tools use this build.)

    # 2. Cross build for the 3DS. Data is packaged as an archive (icudt73l.dat, filtered by
    #    deps/icu/icufilters.json). The port registers it at startup with udata_setCommonData().
    #    ("static" packaging needs the host pkgdata to generate C sources, and the MinGW-host pkgdata
    #    builds broken paths like ./out/tmpicudt73l_dat.c.)
    cp "$src/config/mh-linux" "$src/config/mh-unknown"
    rm -rf "$cross"; mkdir -p "$cross"
    local arch="-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -mword-relocations"
    (
        cd "$cross"
        # The data step runs the host (MinGW) ICU tools, which need the UCRT64 runtime DLLs.
        export PATH="$PATH:/ucrt64/bin"
        export ICU_DATA_FILTER_FILE="$DEPS/icu/icufilters.json"
        export CC="$DEVKITARM/bin/arm-none-eabi-gcc" CXX="$DEVKITARM/bin/arm-none-eabi-g++"
        export AR="$DEVKITARM/bin/arm-none-eabi-ar" RANLIB="$DEVKITARM/bin/arm-none-eabi-ranlib"
        export CPPFLAGS="$INT32_FLAG -D__3DS__ -I$DEVKITPRO/libctru/include -DU_HAVE_MMAP=0 -DU_ENABLE_DYLOAD=0 -DU_TIMEZONE=0"
        export CFLAGS="$arch -O2 -ffunction-sections -fdata-sections"
        export CXXFLAGS="$arch -O2 -ffunction-sections -fdata-sections -std=gnu++17"
        export LDFLAGS="-specs=3dsx.specs $arch -L$DEVKITPRO/libctru/lib"
        export LIBS="-lctru"
        "$src/configure" --host=arm-none-eabi --with-cross-build="$host" --prefix="$PREFIX" \
            --enable-static --disable-shared --disable-tools --disable-tests --disable-samples \
            --disable-extras --disable-icuio --disable-dyload --with-data-packaging=archive
        make -j"$JOBS"
        make install
    )
}

# The ICU data the game embeds (M19: it was built but never registered, so every message with a number lost it):
# the filtered archive cut down to deps/icu/icu-keep.txt by the host icupkg, ~0.9 MB instead of 2.7.
build_icudata() {
    local v=73.2 tools="$BUILD/icu-host"
    # The output keeps the archive's file name: icupkg names the package after it, and ICU looks items up as
    # icudt73l/<item> (an icudt73l_en.dat held items nothing could find).
    local in="$PREFIX/share/icu/$v/icudt73l.dat" out="$PREFIX/share/icu/$v/en/icudt73l.dat"
    local remove="$BUILD/icu-remove.txt"
    export PATH="$tools/lib:/ucrt64/bin:$PATH"
    "$tools/bin/icupkg" -l "$in" | tr '\r' '\n' | grep . | grep -v -x -F -f <(grep -v '^#' "$DEPS/icu/icu-keep.txt" | grep .) > "$remove"
    rm -f "$out"
    mkdir -p "$(dirname "$out")"
    "$tools/bin/icupkg" -r "$remove" "$in" "$out"
    ls -l "$out"
}

# mpg123 with its integer decoder and ARM assembly synth (arm_nofpu): devkitPro's 3ds-mpg123 is the generic
# floating-point decoder, ~16% of a core for one 44.1 kHz stereo stream in Azahar (M7).
build_mpg123() {
    local v=1.32.10
    fetch "https://www.mpg123.de/download/mpg123-$v.tar.bz2" "mpg123-$v.tar.bz2"
    unpack "mpg123-$v.tar.bz2" "mpg123-$v"
    local src="$SRC/mpg123-$v" out="$BUILD/mpg123"
    local arch="-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -mword-relocations"
    rm -rf "$out"; mkdir -p "$out"
    (
        cd "$out"
        export CC="$DEVKITARM/bin/arm-none-eabi-gcc"
        export AR="$DEVKITARM/bin/arm-none-eabi-ar" RANLIB="$DEVKITARM/bin/arm-none-eabi-ranlib"
        export CPPFLAGS="-D__3DS__ -I$DEVKITPRO/libctru/include"
        export CFLAGS="$arch -O2 -ffunction-sections -fdata-sections"
        export LDFLAGS="-specs=3dsx.specs $arch -L$DEVKITPRO/libctru/lib"
        export LIBS="-lctru"
        "$src/configure" --host=arm-none-eabi --prefix="$PREFIX" --enable-static --disable-shared \
            --with-cpu=arm_nofpu --disable-components --enable-libmpg123 --disable-modules \
            --disable-network --disable-largefile
        make -j"$JOBS"
        make install
    )
}

# citro3d (the GPU library, zlib licence), rebuilt from the release devkitPro ships (citro3d 1.7.1-2) with our
# patches (perf batch 5: its per-draw costs, deps/patches/citro3d). Only the library: its headers are unchanged,
# the devkitPro ones are used. Linked from $PREFIX/lib instead of devkitPro's (components/CMakeLists.txt).
build_citro3d() {
    local v=1.7.1
    fetch "https://github.com/devkitPro/citro3d/archive/refs/tags/v$v.tar.gz" "citro3d-$v.tar.gz"
    unpack "citro3d-$v.tar.gz" citro3d
    make -C "$SRC/citro3d" -j"$JOBS" lib/libcitro3d.a
    mkdir -p "$PREFIX/lib"
    cp "$SRC/citro3d/lib/libcitro3d.a" "$PREFIX/lib/libcitro3d.a"
}

ALL_COMPONENTS=(compat sdl2 boost bulletfloat stubgl osg mygui recast sqlite3 yamlcpp icu icudata mpg123 citro3d)

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

