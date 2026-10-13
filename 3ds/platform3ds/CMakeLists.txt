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
)

target_include_directories(platform3ds PUBLIC include)
target_compile_definitions(platform3ds PUBLIC
    OPENMW_PLATFORM_3DS=1
)
target_compile_options(platform3ds PRIVATE -Wall -Wextra -Wno-unused-parameter)

# Frame pointers make the crash handler's backtrace walk reliable.
target_compile_options(platform3ds PUBLIC -fno-omit-frame-pointer)

# Route abort() through the crash logger.
target_link_options(platform3ds INTERFACE -Wl,--wrap=abort)

target_link_libraries(platform3ds PUBLIC ctru m)
