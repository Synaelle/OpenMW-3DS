# Initial cache for configuring openmw/ for the Nintendo 3DS:
#   cmake -C cmake/openmw-3ds-cache.cmake -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake openmw
# (scripts/build-openmw.ps1 does this.)

set(OPENMW_PLATFORM_3DS ON CACHE BOOL "Build the Nintendo 3DS port")
get_filename_component(_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(OPENMW3DS_ROOT "${_root}" CACHE PATH "Root of the openmw3ds repository")
set(OPENMW3DS_DEPS_PREFIX "${_root}/.deps/3ds" CACHE PATH "Cross-built dependency prefix")

# Only the game.
set(BUILD_OPENMW ON CACHE BOOL "")
foreach(_tool LAUNCHER WIZARD MWINIIMPORTER OPENCS ESSIMPORTER BSATOOL ESMTOOL NIFTEST NAVMESHTOOL
              BULLETOBJECTTOOL COMPONENTS_TESTS OPENMW_TESTS OPENCS_TESTS BENCHMARKS)
    set(BUILD_${_tool} OFF CACHE BOOL "")
endforeach()

# Every dependency comes from .deps/3ds (scripts/build-deps.ps1), statically linked.
foreach(_dep OSG MYGUI BULLET RECASTNAVIGATION SQLITE3 YAML_CPP ICU)
    set(OPENMW_USE_SYSTEM_${_dep} ON CACHE BOOL "")
endforeach()
set(OSG_STATIC ON CACHE BOOL "")
set(MYGUI_STATIC ON CACHE BOOL "")
set(BULLET_STATIC ON CACHE BOOL "")
# Bullet precision. Single by default on the 3DS: a collision sweep cost ~0.8 ms in double precision on the
# ARM11. Double is upstream's requirement and stays available: both libraries are built (build-deps.ps1:
# "bullet" in .deps/3ds, "bulletfloat" in .deps/3ds-bullet-float). Switch with build-openmw.ps1 -BulletDouble
# (passes this option on every configure; everything recompiles). Nothing saved depends on the precision.
set(OPENMW3DS_BULLET_DOUBLE OFF CACHE BOOL "Link double-precision Bullet (upstream) instead of single precision")
set(OPENMW3DS_DEBUG ON CACHE BOOL "3DS debug tools (debug bottom screen, autorun, detailed profiling); OFF for a release")
set(OPENMW3DS_BULLET_FLOAT_PREFIX "${_root}/.deps/3ds-bullet-float" CACHE PATH "Single-precision Bullet prefix")
set(RECASTNAVIGATION_STATIC ON CACHE BOOL "")
set(YAML_CPP_STATIC ON CACHE BOOL "")
set(BOOST_STATIC ON CACHE BOOL "")
set(SDL2_STATIC ON CACHE BOOL "")

set(USE_LUAJIT OFF CACHE BOOL "PUC Lua 5.1 from devkitPro portlibs")
set(OPENMW_LTO_BUILD OFF CACHE BOOL "")
set(OPENMW_UNITY_BUILD OFF CACHE BOOL "")

set(CMAKE_PREFIX_PATH "${_root}/.deps/3ds" CACHE STRING "")
set(CMAKE_FIND_ROOT_PATH "${_root}/.deps/3ds;/opt/devkitpro/portlibs/3ds;/opt/devkitpro/libctru" CACHE STRING "")
set(OPENGL_INCLUDE_DIR "${_root}/.deps/3ds/include" CACHE PATH "stub GL")
set(OPENGL_gl_LIBRARY "${_root}/.deps/3ds/lib/libGL.a" CACHE FILEPATH "stub GL")
set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON CACHE BOOL "MSYS paths in generated PCH headers break the native compiler")

# OpenMW links its static OSG plugins with $<LINK_LIBRARY:WHOLE_ARCHIVE,...>; the devkitPro
# platform module doesn't define that feature, so use GNU ld's flags.
set(CMAKE_LINK_LIBRARY_USING_WHOLE_ARCHIVE "LINKER:--whole-archive" "<LINK_ITEM>" "LINKER:--no-whole-archive" CACHE INTERNAL "")
set(CMAKE_LINK_LIBRARY_USING_WHOLE_ARCHIVE_SUPPORTED TRUE CACHE INTERNAL "")

# Static link of ~45 libraries with circular references (OSG serializer plugins -> libosg, ICU
# i18n -> uc, ...): put them all in one linker group instead of relying on CMake's ordering.
set(CMAKE_CXX_LINK_EXECUTABLE "<CMAKE_CXX_COMPILER> <FLAGS> <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> -Wl,--start-group <LINK_LIBRARIES> -Wl,--end-group" CACHE STRING "" FORCE)
