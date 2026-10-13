cmake_minimum_required(VERSION 3.20)

# Superbuild for the OpenMW Nintendo 3DS port.
# Configure with: -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake (see scripts/build.ps1)
project(openmw3ds C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

# GCC's 'parameter passing changed in GCC 9.1' ABI notes are irrelevant for a static executable.
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Wno-psabi>)

if(NOT NINTENDO_3DS)
    message(FATAL_ERROR "This tree must be configured with the devkitPro 3DS toolchain file")
endif()

option(OPENMW3DS_BUILD_TESTAPP "Build the M0 platform test app" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Ctr3dsPackaging)

add_subdirectory(platform3ds)

if(OPENMW3DS_BUILD_TESTAPP)
    add_subdirectory(testapp)
endif()
