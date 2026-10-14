# platform3ds: the port-specific platform layer (logging, crash handler, memory stats,
# bottom-screen modules). Kept outside the OpenMW tree so the upstream diff stays small.
add_library(platform3ds STATIC
    src/platform.cpp
    src/log.cpp
    src/crash.cpp
    src/memstats.cpp
    src/textfb.cpp
    src/debugscreen.cpp
    src/screenshot.cpp
    src/memprobe.cpp
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
target_compile_options(platform3ds PRIVATE -Wall -Wextra -Wno-unused-parameter)

# Frame pointers make the crash handler's backtrace walk reliable.
target_compile_options(platform3ds PUBLIC -fno-omit-frame-pointer)

# Route abort() through the crash logger; give pthreads/std::thread a usable default stack
# (libctru's is 32 KB, see platform.cpp).
target_link_options(platform3ds INTERFACE -Wl,--wrap=abort -Wl,--wrap=__syscall_thread_create)

target_link_libraries(platform3ds PUBLIC ctru m)
