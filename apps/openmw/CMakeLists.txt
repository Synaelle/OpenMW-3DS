set(OPENMW_SOURCES
    engine.cpp
    options.cpp
)

set(OPENMW_RESOURCES
    ${CMAKE_SOURCE_DIR}/files/windows/openmw.rc
    ${CMAKE_SOURCE_DIR}/files/windows/openmw.exe.manifest
)

set(OPENMW_HEADERS
    doc.hpp
    engine.hpp
    options.hpp
    profile.hpp
)

source_group(apps/openmw FILES main.cpp androidmain.cpp ${OPENMW_SOURCES} ${OPENMW_HEADERS} ${OPENMW_RESOURCES})

add_openmw_dir (mwrender
    actors objects renderingmanager animation rotatecontroller sky skyutil npcanimation esm4npcanimation vismask
    creatureanimation effectmanager util renderinginterface pathgrid rendermode weaponanimation screenshotmanager
    bulletdebugdraw globalmap characterpreview camera localmap water terrainstorage ripplesimulation
    renderbin actoranimation landmanager navmesh actorspaths recastmesh fogmanager objectpaging groundcover
    postprocessor pingpongcull luminancecalculator pingpongcanvas transparentpass opaqueblit waterawaretransparentbin precipitationocclusion ripples
    actorutil distortion animationpriority bonegroup blendmask animblendcontroller
    )

add_openmw_dir (mwinput
    actions actionmanager bindingsmanager controllermanager controlswitch
    inputmanagerimp mousemanager keyboardmanager sensormanager gyromanager
    )

add_openmw_dir (mwgui
    layout textinput widgets race class birth review windowmanagerimp console dialogue
    windowbase statswindow messagebox journalwindow charactercreation
    mapwindow windowpinnablebase tooltips scrollwindow bookwindow resourceskin
    formatting inventorywindow container hud countdialog tradewindow settingswindow
    confirmationdialog alchemywindow referenceinterface spellwindow mainmenu quickkeysmenu
    itemselection spellbuyingwindow loadingscreen levelupdialog waitdialog spellcreationdialog
    enchantingdialog trainingwindow travelwindow exposedwindow cursor spellicons
    merchantrepair repair soulgemdialog companionwindow bookpage journalviewmodel journalbooks
    itemmodel containeritemmodel inventoryitemmodel sortfilteritemmodel itemview
    tradeitemmodel companionitemmodel pickpocketitemmodel controllers savegamedialog
    recharge mode videowidget backgroundimage itemwidget screenfader debugwindow spellmodel spellview
    draganddrop timeadvancer jailscreen itemchargeview keyboardnavigation textcolours statswatcher
    postprocessorhud settings worlditemmodel itemtransfer controllerbuttonsoverlay inventorytabsoverlay
    )

add_openmw_dir (mwdialogue
    dialoguemanagerimp journalimp journalentry quest topic filter selectwrapper keywordsearch scripttest
    )

add_openmw_dir (mwscript
    locals scriptmanagerimp compilercontext interpretercontext cellextensions miscextensions
    guiextensions soundextensions skyextensions statsextensions containerextensions
    aiextensions controlextensions extensions globalscripts ref dialogueextensions
    animationextensions transformationextensions consoleextensions userextensions
    )

add_openmw_dir (mwlua
    objectcache luamanagerimp object objectlists userdataserializer luaevents engineevents objectvariant loadscripts contentbindings
    context menuscripts globalscripts localscripts playerscripts luabindings objectbindings cellbindings coremwscriptbindings
    mwscriptbindings camerabindings vfsbindings uibindings soundbindings inputbindings nearbybindings dialoguebindings
    postprocessingbindings stats recordstore debugbindings corebindings worldbindings worker landbindings magicbindings factionbindings
    classbindings itemdata inputprocessor animationbindings birthsignbindings racebindings markupbindings weatherbindings regionbindings
    types/types types/door types/item types/actor types/container types/lockable types/weapon types/npc types/bodypart
    types/creature types/player types/activator types/book types/lockpick types/probe types/apparatus
    types/potion types/ingredient types/misc types/repair types/armor types/light types/static
    types/clothing types/levelledlist types/terminal magictypebindings
    )

add_openmw_dir (mwsound
    soundmanagerimp openaloutput ffmpegdecoder headcache warmqueue sound soundbuffer sounddecoder soundoutput
    loudness movieaudiofactory alext efx efxpresets regionsoundselector watersoundupdater
    )

add_openmw_dir (mwworld
    refdata worldimp scene globals class action nullaction actionteleport
    containerstore actiontalk actiontake manualref player cellvisitors failedaction
    worldmodel localscripts customdata inventorystore ptr actionopen actionread actionharvest
    actionequip timestamp actionalchemy cellstore actionapply actioneat
    store esmstore fallback actionrepair actionsoulgem livecellref actiondoor
    contentloader esmloader actiontrap cellreflist cellref weather projectilemanager
    cellpreloader datetimemanager groundcoverstore magiceffects cell ptrregistry
    positioncellgrid
    )

add_openmw_dir (mwphysics
    physicssystem trace collisiontype actor convert object heightfield closestnotmerayresultcallback
    contacttestresultcallback stepper movementsolver projectile
    actorconvexcallback raycasting mtphysics contacttestwrapper projectileconvexcallback
    )

add_openmw_dir (mwclass
    classes activator creature npc weapon armor potion apparatus book clothing container door
    ingredient creaturelevlist itemlevlist light lockpick misc probe repair static actor bodypart
    esm4base esm4npc light4
    )

add_openmw_dir (mwmechanics
    mechanicsmanagerimp stat creaturestats magiceffects movement actorutil spelllist
    drawstate spells activespells npcstats aipackage aisequence aipursue alchemy aiwander aitravel aifollow aiavoiddoor aibreathe
    aicast aiescort aiface aiactivate aicombat recharge repair enchanting pathfinding pathgrid security spellcasting spellresistance
    disease pickpocket levelledlist combat steering obstacle autocalcspell difficultyscaling aicombataction summoning
    character actors objects aistate weaponpriority spellpriority weapontype spellutil
    spelleffects
    )

add_openmw_dir (mwstate
    statemanagerimp charactermanager character quicksavemanager
    )

add_openmw_dir (mwbase
    environment world scriptmanager dialoguemanager journal soundmanager mechanicsmanager
    inputmanager windowmanager statemanager luamanager
    )

# Main executable

if (OPENMW_PLATFORM_3DS)
    # Nintendo 3DS: no OpenAL, FFmpeg or video playback. Replacements live in apps/openmw/n3ds/: sound through the
    # DSP (NDSP) with WAV and MP3 (mpg123) decoding.
    list(FILTER OPENMW_FILES EXCLUDE REGEX "mwsound/(openaloutput|ffmpegdecoder|movieaudiofactory|alext|efx|efxpresets)\\.[ch]pp$")
    list(FILTER OPENMW_FILES EXCLUDE REGEX "mwgui/videowidget\\.cpp$")
    list(APPEND OPENMW_FILES n3ds/sound3ds.hpp n3ds/ndspoutput.cpp n3ds/sounddecoder3ds.cpp n3ds/videowidget_null.cpp
        n3ds/main3ds.cpp n3ds/frame3ds.cpp n3ds/hydration.cpp n3ds/menus.cpp n3ds/hud3ds.cpp n3ds/memrelief.cpp
        n3ds/credits3ds.cpp)
    if (OPENMW3DS_DEBUG) # debug tools: autorun scripts, scripted input, teleport buttons
        list(APPEND OPENMW_FILES n3ds/debugtools.cpp n3ds/hidinject.cpp n3ds/benchmark3ds.cpp)
    endif()
    # Per-frame hot code at -O2 (the rest stays -Os: image size is memory): animation, physics, AI.
    set(OMW3DS_HOT_FILES ${OPENMW_FILES})
    list(FILTER OMW3DS_HOT_FILES INCLUDE REGEX "/(mwrender|mwphysics|mwmechanics)/[^/]*\\.cpp$")
    set_source_files_properties(${OMW3DS_HOT_FILES} PROPERTIES COMPILE_OPTIONS "-O2")
endif()

add_library(openmw-lib STATIC
    ${OPENMW_FILES}
    ${OPENMW_SOURCES}
)

if (MSVC)
    # these TUs take a lot of memory for codegen
    # in CI, they often go over the 8GB memory limit if another cl.exe is running at the same time
    # this option (make sure you're not looking at the docs for the identically-named linker option) stops four codegen threads being spun off
    # it hurts build time ~33% to enable this unnecessarily, though, so target it to the hungriest boys
    set_source_files_properties(
        mwlua/dialoguebindings.cpp
        mwlua/magicbindings.cpp
        mwlua/objectbindings
        mwlua/stats.cpp
        mwworld/esmstore.cpp
        PROPERTIES
        COMPILE_OPTIONS /cgthreads1
    )
endif()

if(BUILD_OPENMW)
    if (ANDROID)
        add_library(openmw SHARED
            main.cpp
            androidmain.cpp
        )
    else()
        openmw_add_executable(openmw
            ${APPLE_BUNDLE_RESOURCES}
            ${OPENMW_RESOURCES}
            main.cpp
        )
    endif()

    target_link_libraries(openmw openmw-lib)

    if (OPENMW_PLATFORM_3DS)
        include(${OPENMW3DS_ROOT}/cmake/Ctr3dsPackaging.cmake)
        omw3ds_package(openmw
            TITLE "OpenMW"
            DESCRIPTION "OpenMW engine port for New 3DS" # no game name in the title metadata (the tester, 20/10)
            AUTHOR "OpenMW 3DS port"
            PRODUCT_CODE "CTR-P-OMW3"
            UNIQUE_ID 0xF6D3E # must match n3ds/main3ds.cpp
            SYSTEM_MODE_EXT 178MB)
        # Scripted input for automated emulator runs (n3ds/hidinject.cpp, debug tools).
        if (OPENMW3DS_DEBUG)
            target_link_options(openmw PRIVATE
                "LINKER:--wrap=hidKeysDown,--wrap=hidKeysUp,--wrap=hidKeysHeld,--wrap=hidCircleRead,--wrap=irrstCstickRead,--wrap=hidTouchRead")
        endif()
    endif()

    # Workaround necessary to ensure osgAnimation::MatrixLinearSampler dynamic casts work under Clang
    # NOTE: it's unclear whether the broken behavior is spec-compliant
    if (CMAKE_CXX_COMPILER_ID STREQUAL Clang)
        set_target_properties(openmw PROPERTIES ENABLE_EXPORTS ON)
    endif()
endif()

# Sound stuff - here so CMake doesn't stupidly recompile EVERYTHING
# when we change the backend.
include_directories(
    ${FFmpeg_INCLUDE_DIRS}
)

target_link_libraries(openmw-lib
    # CMake's built-in OSG finder does not use pkgconfig, so we have to
    # manually ensure the order is correct for inter-library dependencies.
    # This only makes a difference with `-DOPENMW_USE_SYSTEM_OSG=ON -DOSG_STATIC=ON`.
    # https://gitlab.kitware.com/cmake/cmake/-/issues/21701
    ${OSGPARTICLE_LIBRARIES}
    ${OSGVIEWER_LIBRARIES}
    ${OSGGA_LIBRARIES}
    ${OSGSHADOW_LIBRARIES}
    ${OSGDB_LIBRARIES}
    ${OSGUTIL_LIBRARIES}
    ${OSG_LIBRARIES}
    Boost::program_options
    ${OPENAL_LIBRARY}
    ${FFmpeg_LIBRARIES}
    ${MyGUI_LIBRARIES}
    SDL2::SDL2
    ${RecastNavigation_LIBRARIES}
    oics
    components
)
if (OPENMW_PLATFORM_3DS)
    # Our mpg123 (deps/build-deps.sh: integer decoder, ARM synth), not devkitPro's floating-point 3ds-mpg123. Its
    # header is found first too (.deps/3ds/include comes before the portlibs).
    # ICU's data (English, deps/build-deps.sh build_icudata), embedded and registered at start-up (main3ds.cpp):
    # ICU's own data library is a stub with archive packaging. 16-byte aligned, as udata_setCommonData needs.
    set(OPENMW3DS_ICU_DATA "${OPENMW3DS_DEPS_PREFIX}/share/icu/73.2/en/icudt73l.dat")
    if (NOT EXISTS "${OPENMW3DS_ICU_DATA}")
        message(FATAL_ERROR "${OPENMW3DS_ICU_DATA} missing: run scripts/build-deps.ps1 icudata")
    endif()
    set(DKP_BIN2S_ALIGNMENT 16)
    dkp_add_embedded_binary_library(openmw3ds_icudata "${OPENMW3DS_ICU_DATA}")
    target_link_libraries(openmw-lib platform3ds openmw3ds_icudata
        ${OPENMW3DS_DEPS_PREFIX}/lib/libcompat3ds.a # POSIX stubs (fchown, geteuid)
        ${OPENMW3DS_DEPS_PREFIX}/lib/libmpg123.a
        bz2)                                       # needed by freetype
else()
    target_link_libraries(openmw-lib osg-ffmpeg-videoplayer)
endif()

if (MSVC AND PRECOMPILE_HEADERS_WITH_MSVC)
    target_precompile_headers(openmw-lib PRIVATE
        <boost/program_options/options_description.hpp>

        <sol/sol.hpp>

        <osg/StateSet>
        <osg/Node>
        <osg/Drawable>
        <osg/Camera>

        <osgViewer/Viewer>

        <MyGUI_Widget.h>

        <algorithm>
        <filesystem>
        <fstream>
        <functional>
        <memory>
        <string>
        <string_view>
        <vector>
    )
endif()

if (ANDROID)
    target_link_libraries(openmw-lib EGL android log z)
endif (ANDROID)

if (USE_SYSTEM_TINYXML)
    target_link_libraries(openmw-lib ${TinyXML_LIBRARIES})
endif()

if (NOT UNIX)
    target_link_libraries(openmw-lib ${SDL2MAIN_LIBRARY})
endif()

# Fix for not visible pthreads functions for linker with glibc 2.15
if (UNIX AND NOT APPLE)
    target_link_libraries(openmw-lib ${CMAKE_THREAD_LIBS_INIT})
endif()

if(APPLE AND BUILD_OPENMW)
    set(BUNDLE_RESOURCES_DIR "${APP_BUNDLE_DIR}/Contents/Resources")

    set(OPENMW_RESOURCES_ROOT ${BUNDLE_RESOURCES_DIR})

    add_subdirectory(../../files/ ${CMAKE_CURRENT_BINARY_DIR}/files)

    configure_file("${OpenMW_BINARY_DIR}/defaults.bin" ${BUNDLE_RESOURCES_DIR} COPYONLY)
    configure_file("${OpenMW_BINARY_DIR}/openmw.cfg" ${BUNDLE_RESOURCES_DIR} COPYONLY)
    configure_file("${OpenMW_BINARY_DIR}/gamecontrollerdb.txt" ${BUNDLE_RESOURCES_DIR} COPYONLY)

    add_custom_command(TARGET openmw
        POST_BUILD
        COMMAND cp "${OpenMW_BINARY_DIR}/resources/version" "${BUNDLE_RESOURCES_DIR}/resources")

    find_library(COCOA_FRAMEWORK Cocoa)
    find_library(IOKIT_FRAMEWORK IOKit)
    target_link_libraries(openmw ${COCOA_FRAMEWORK} ${IOKIT_FRAMEWORK})

    if (FFmpeg_FOUND)
        target_link_options(openmw PRIVATE "LINKER:SHELL:-framework CoreVideo"
                                           "LINKER:SHELL:-framework CoreMedia"
                                           "LINKER:SHELL:-framework VideoToolbox"
                                           "LINKER:SHELL:-framework AudioToolbox"
                                           "LINKER:SHELL:-framework VideoDecodeAcceleration")
    endif()
endif()

if (BUILD_WITH_CODE_COVERAGE)
    target_compile_options(openmw-lib PRIVATE --coverage)
    target_link_libraries(openmw-lib gcov)
    if (NOT ANDROID AND BUILD_OPENMW)
        target_compile_options(openmw PRIVATE --coverage)
        target_link_libraries(openmw gcov)
    endif()
endif()

if (NOT APPLE AND BUILD_OPENMW)
    install(TARGETS openmw ${runtime_dependency_arguments})
endif()
