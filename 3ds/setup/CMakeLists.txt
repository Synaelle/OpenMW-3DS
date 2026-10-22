cmake_minimum_required(VERSION 3.21)
project(openmw3ds-setup VERSION 0.1.1 LANGUAGES CXX)

# The setup app for OpenMW for New 3DS: prepares the folder players copy to their 3DS's SD card from their own
# Morrowind installation. A core library (no interface), the command-line tool on top of it, the window later.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# OpenMW's list of Morrowind.ini keys it imports as fallback values (data/fallback_keys.txt, from
# apps/mwiniimporter/importer.cpp, refreshed by tools/update_fallback_keys.py), embedded as a string array.
file(STRINGS "${CMAKE_SOURCE_DIR}/data/fallback_keys.txt" fallbackKeys)
set(fallbackInc "")
foreach(key IN LISTS fallbackKeys)
    string(APPEND fallbackInc "    \"${key}\",\n")
endforeach()
file(WRITE "${CMAKE_BINARY_DIR}/generated/fallback_keys.inc" "${fallbackInc}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/data/fallback_keys.txt")

# The texture names OpenMW's interface code uses (data/ui_code_textures.txt, refreshed by
# tools/update_ui_code_textures.py): they go in the UI atlas.
file(STRINGS "${CMAKE_SOURCE_DIR}/data/ui_code_textures.txt" codeTextures)
set(codeTexturesInc "")
foreach(name IN LISTS codeTextures)
    string(APPEND codeTexturesInc "    \"${name}\",\n")
endforeach()
file(WRITE "${CMAKE_BINARY_DIR}/generated/ui_code_textures.inc" "${codeTexturesInc}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/data/ui_code_textures.txt")

add_library(setupcore STATIC
    src/core/bleed.cpp
    src/core/bsawriter.cpp
    src/core/cooker.cpp
    src/core/decode.cpp
    src/core/detect.cpp
    src/core/esm.cpp
    src/core/fallbacks.cpp
    src/core/image.cpp
    src/core/pica.cpp
    src/core/prepare.cpp
    src/core/resample.cpp
    src/core/text.cpp
    src/core/update.cpp
    third_party/rg_etc1/rg_etc1.cpp
)
target_include_directories(setupcore PUBLIC src PRIVATE third_party "${CMAKE_BINARY_DIR}/generated")
target_compile_options(setupcore PRIVATE -Wall -Wextra)
# The cooker reproduces the Python cooker's float32 arithmetic (numpy) operation for operation: no fused
# multiply-adds.
target_compile_options(setupcore PRIVATE -ffp-contract=off)
set_source_files_properties(third_party/rg_etc1/rg_etc1.cpp PROPERTIES COMPILE_OPTIONS "-w")
find_package(Threads REQUIRED)
target_link_libraries(setupcore PUBLIC Threads::Threads)
if(WIN32)
    # The registry (Steam, GOG), the desktop folder.
    target_link_libraries(setupcore PUBLIC advapi32 shell32 ole32 uuid winhttp)
endif()

add_executable(openmw3ds-setup-cli src/cli/main.cpp)
target_link_libraries(openmw3ds-setup-cli PRIVATE setupcore)
target_compile_options(openmw3ds-setup-cli PRIVATE -Wall -Wextra)
if(MINGW)
    # One executable, nothing to install next to it.
    target_link_options(openmw3ds-setup-cli PRIVATE -static -municode)
endif()

# The window: Dear ImGui (third_party/imgui) over SDL2's renderer, SDL2 linked statically.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
add_executable(openmw3ds-setup WIN32
    src/gui/main.cpp
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/imgui_impl_sdl2.cpp
    third_party/imgui/imgui_impl_sdlrenderer2.cpp
)
if(WIN32)
    target_sources(openmw3ds-setup PRIVATE src/gui/system_win.cpp)
    target_link_libraries(openmw3ds-setup PRIVATE dwmapi)
else()
    target_sources(openmw3ds-setup PRIVATE src/gui/system_unix.cpp)
endif()
target_include_directories(openmw3ds-setup PRIVATE src third_party third_party/imgui ${SDL2_INCLUDE_DIRS})
target_compile_options(openmw3ds-setup PRIVATE ${SDL2_CFLAGS_OTHER})
target_link_libraries(openmw3ds-setup PRIVATE setupcore)
if(MINGW)
    target_link_options(openmw3ds-setup PRIVATE -static)
    target_link_libraries(openmw3ds-setup PRIVATE ${SDL2_STATIC_LDFLAGS})
else()
    target_link_libraries(openmw3ds-setup PRIVATE ${SDL2_LDFLAGS})
endif()
