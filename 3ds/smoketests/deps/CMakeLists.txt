# M1 dependency smoke test: links the cross-built OSG / Bullet / Boost / SDL2 and exercises each.
set(P "${OPENMW3DS_DEPS_PREFIX}")

add_executable(depsmoke main.cpp)

# Filtered ICU data archive (1.8 MB), embedded and registered with udata_setCommonData() at startup.
set(DKP_BIN2S_ALIGNMENT 16)
dkp_add_embedded_binary_library(depsmoke_icudata "${P}/share/icu/73.2/icudt73l.dat")
target_include_directories(depsmoke PRIVATE
    "${P}/include" "${P}/include/bullet" "${P}/include/MYGUI" "${P}/include/recastnavigation"
    "${DEVKITPRO}/portlibs/3ds/include/lua5.1"
    "${CMAKE_SOURCE_DIR}/openmw/extern/sol3")   # same sol3 headers OpenMW uses
# BT_USE_DOUBLE_PRECISION must match the Bullet build (BulletConfig.cmake BULLET_DEFINITIONS):
# without it btScalar is float here and double in the library -> corrupt layouts / vtables.
target_compile_definitions(depsmoke PRIVATE BOOST_HAS_PTHREADS BT_USE_DOUBLE_PRECISION)
target_compile_options(depsmoke PRIVATE -Wall -Wno-unused-parameter)

# Static link order matters: dependents first.
target_link_libraries(depsmoke PRIVATE
    platform3ds depsmoke_icudata
    "${P}/lib/libosgUtil.a" "${P}/lib/libosg.a" "${P}/lib/libOpenThreads.a" "${P}/lib/libGL.a"
    "${P}/lib/libBulletCollision.a" "${P}/lib/libLinearMath.a"
    "${P}/lib/libboost_program_options.a" "${P}/lib/libboost_container.a"
    "${P}/lib/libSDL2.a"
    "${P}/lib/libicui18n.a" "${P}/lib/libicuuc.a" "${P}/lib/libicudata.a"
    "${P}/lib/libMyGUIEngineStatic.a"
    "${P}/lib/libDetour.a" "${P}/lib/libRecast.a"
    "${P}/lib/libsqlite3.a"
    "${P}/lib/libcompat3ds.a"
    "${P}/lib/libyaml-cpp.a"
    "${DEVKITPRO}/portlibs/3ds/lib/liblua5.1.a"
    "${DEVKITPRO}/portlibs/3ds/lib/libfreetype.a" "${DEVKITPRO}/portlibs/3ds/lib/libpng16.a"
    "${DEVKITPRO}/portlibs/3ds/lib/libbz2.a" "${DEVKITPRO}/portlibs/3ds/lib/libz.a"
    citro3d ctru m)

dkp_target_generate_symbol_list(depsmoke)
omw3ds_package(depsmoke
    TITLE "OpenMW3DS depsmoke"
    DESCRIPTION "M1 dependency smoke test"
    AUTHOR "OpenMW 3DS port"
    PRODUCT_CODE "CTR-P-OMWD"
    UNIQUE_ID 0xF6D31
    SYSTEM_MODE_EXT 178MB)
