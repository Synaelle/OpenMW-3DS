# Minimal build of the SQLite amalgamation (OpenMW only uses it for the navmesh disk cache).
cmake_minimum_required(VERSION 3.16)
project(sqlite3 C)
add_library(sqlite3 STATIC sqlite3.c)
# No threads needed by OpenMW's usage on 3DS. No dlopen / mmap / WAL shared memory on newlib.
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_OMIT_WAL
    SQLITE_MAX_MMAP_SIZE=0
    SQLITE_OMIT_DEPRECATED
    SQLITE_DEFAULT_MEMSTATUS=0
    HAVE_USLEEP=1)
install(TARGETS sqlite3 ARCHIVE DESTINATION lib)
install(FILES sqlite3.h sqlite3ext.h DESTINATION include)
