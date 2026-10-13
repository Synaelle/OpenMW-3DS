# Writes ${OUT} with a fresh build id (UTC time + git revision). Run at every build.
execute_process(COMMAND git rev-parse --short HEAD WORKING_DIRECTORY "${SRC}" OUTPUT_VARIABLE rev
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND git status --porcelain --untracked-files=no WORKING_DIRECTORY "${SRC}" OUTPUT_VARIABLE dirty
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(dirty)
    set(rev "${rev}+dirty")
endif()
string(TIMESTAMP now "%Y%m%d-%H%M%S" UTC)
set(content "const char* const p3ds_build_id = \"${now} ${rev}\";\n")
if(EXISTS "${OUT}")
    file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL content)
    file(WRITE "${OUT}" "${content}")
endif()
