cmake_minimum_required(VERSION 3.20)

# Superbuild for the OpenMW Nintendo 3DS port.
# Configure with: -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake (see scripts/build.ps1)
project(openmw3ds C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

# int32_t = int ABI across the whole port and every C++ dependency (see deps/compat/include/openmw3ds_int32.h).
add_compile_options("SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/deps/compat/include/openmw3ds_int32.h")

# GCC's 'parameter passing changed in GCC 9.1' ABI notes are irrelevant for a static executable.
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Wno-psabi>)

if(NOT NINTENDO_3DS)
    message(FATAL_ERROR "This tree must be configured with the devkitPro 3DS toolchain file")
endif()

set(OPENMW3DS_DEPS_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/.deps/3ds" CACHE PATH "Cross-built dependency prefix")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Ctr3dsPackaging)

add_subdirectory(platform3ds)


