# platform3ds: the port-specific platform layer (logging, crash handler, memory stats,
# bottom-screen modules). Kept outside the OpenMW tree so the upstream diff stays small.
add_library(platform3ds STATIC
    src/platform.cpp
    src/log.cpp
    src/crash.cpp
    src/memstats.cpp
    src/textfb.cpp
    src/loadscreen.cpp
    src/menuart.cpp
    src/debugscreen.cpp
    src/screenshot.cpp
    src/memprobe.cpp
    src/pools.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/buildinfo.c
)

# Unique id per build, printed in the log and in crash reports, so a crash report can always be
# matched to the right .elf (scripts/symbolize.ps1 also matches by image range).
add_custom_target(platform3ds_buildinfo
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/.. -DOUT=${CMAKE_CURRENT_BINARY_DIR}/buildinfo.c
            -P ${CMAKE_CURRENT_SOURCE_DIR}/../cmake/GenBuildInfo.cmake
    BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/buildinfo.c)
add_dependencies(platform3ds platform3ds_buildinfo)

target_include_directories(platform3ds PUBLIC include)
target_compile_definitions(platform3ds PUBLIC
    OPENMW_PLATFORM_3DS=1
)

# Debug tools, for test builds: the bottom screen's FPS / position / teleport display, autorun scripts and
# scripted input, the detailed profiling log lines (perf/more, gpu/vs, physics, per-insert breakdowns, SD read
# sizes, cull phases) and screenshot view lines. OFF for a release: all of it compiles out (OPENMW3DS_DEBUG
# undefined). The log file, crash reports, the watchdog, the GPU self-test and the 5 s perf/memory summary
# lines stay either way.
option(OPENMW3DS_DEBUG "3DS debug tools (debug bottom screen, autorun, detailed profiling)" ON)
if (OPENMW3DS_DEBUG)
    target_compile_definitions(platform3ds PUBLIC OPENMW3DS_DEBUG=1)
endif()
target_compile_options(platform3ds PRIVATE -Wall -Wextra -Wno-unused-parameter)

# Frame pointers make the crash handler's backtrace walk reliable.
target_compile_options(platform3ds PUBLIC -fno-omit-frame-pointer)

# Route abort() through the crash logger; give pthreads/std::thread a usable default stack
# (libctru's is 32 KB, see platform.cpp); one lock for libctru's linear allocator (platform.cpp).
target_link_options(platform3ds INTERFACE -Wl,--wrap=abort -Wl,--wrap=__syscall_thread_create
    -Wl,--wrap=linearAlloc -Wl,--wrap=linearMemAlign -Wl,--wrap=linearRealloc -Wl,--wrap=linearGetSize
    -Wl,--wrap=linearFree -Wl,--wrap=linearSpaceFree
    # Memory pools beside newlib's malloc (pools.cpp).
    -Wl,--wrap=malloc -Wl,--wrap=free -Wl,--wrap=realloc -Wl,--wrap=calloc -Wl,--wrap=malloc_usable_size)

target_link_libraries(platform3ds PUBLIC ctru m)
