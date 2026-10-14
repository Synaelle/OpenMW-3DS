# Packaging helpers: .3dsx (via devkitPro's ctr_create_3dsx) and .cia (bannertool + makerom).
# makerom/bannertool are fetched into .tools/bin by scripts/bootstrap.ps1.

# Paths are relative to this file: the module is used both by the repo superbuild and by openmw/.
get_filename_component(_omw3ds_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
find_program(OMW3DS_MAKEROM makerom HINTS "${_omw3ds_root}/.tools/bin")
find_program(OMW3DS_BANNERTOOL bannertool HINTS "${_omw3ds_root}/.tools/bin")

set(OMW3DS_PACKAGING_DIR "${_omw3ds_root}/packaging")

# omw3ds_package(<elf target>
#     TITLE <name> DESCRIPTION <text> AUTHOR <text>
#     PRODUCT_CODE <CTR-P-XXXX> UNIQUE_ID <0xFxxxx>
#     [SYSTEM_MODE_EXT 124MB|178MB])
# Produces <target>.3dsx always and <target>.cia when makerom/bannertool are available.
function(omw3ds_package target)
    cmake_parse_arguments(PARSE_ARGV 1 P "" "TITLE;DESCRIPTION;AUTHOR;PRODUCT_CODE;UNIQUE_ID;SYSTEM_MODE_EXT" "")
    if(NOT P_SYSTEM_MODE_EXT)
        set(P_SYSTEM_MODE_EXT 178MB)
    endif()

    set(icon_png "${OMW3DS_PACKAGING_DIR}/icon.png")
    set(smdh "${CMAKE_CURRENT_BINARY_DIR}/${target}.smdh")
    ctr_generate_smdh(OUTPUT "${smdh}" NAME "${P_TITLE}" DESCRIPTION "${P_DESCRIPTION}" AUTHOR "${P_AUTHOR}" ICON "${icon_png}")
    ctr_create_3dsx(${target} SMDH "${smdh}")

    if(NOT OMW3DS_MAKEROM OR NOT OMW3DS_BANNERTOOL)
        message(WARNING "makerom/bannertool not found: skipping .cia for ${target} (run scripts/bootstrap.ps1)")
        return()
    endif()

    set(cia_smdh "${CMAKE_CURRENT_BINARY_DIR}/${target}.cia.smdh")
    set(banner "${CMAKE_CURRENT_BINARY_DIR}/${target}.bnr")
    set(cia "${CMAKE_CURRENT_BINARY_DIR}/${target}.cia")

    add_custom_command(OUTPUT "${cia_smdh}"
        COMMAND "${OMW3DS_BANNERTOOL}" makesmdh -s "${P_TITLE}" -l "${P_DESCRIPTION}" -p "${P_AUTHOR}"
                -i "${icon_png}" -o "${cia_smdh}"
        DEPENDS "${icon_png}" VERBATIM)
    # HOME Menu banner: a flat image and a swoosh (packaging/make_assets.py). Not a 3D CGFX banner: one that
    # worked in the emulator crashed the HOME Menu on hardware.
    set(banner_png "${OMW3DS_PACKAGING_DIR}/banner.png")
    set(banner_wav "${OMW3DS_PACKAGING_DIR}/banner.wav")
    add_custom_command(OUTPUT "${banner}"
        COMMAND "${OMW3DS_BANNERTOOL}" makebanner -i "${banner_png}" -a "${banner_wav}" -o "${banner}"
        DEPENDS "${banner_png}" "${banner_wav}" VERBATIM)
    add_custom_command(OUTPUT "${cia}"
        COMMAND "${OMW3DS_MAKEROM}" -f cia -o "${cia}" -target t -exefslogo
                -elf "$<TARGET_FILE:${target}>" -rsf "${OMW3DS_PACKAGING_DIR}/app.rsf"
                -icon "${cia_smdh}" -banner "${banner}"
                "-DAPP_TITLE=${P_TITLE}" "-DAPP_PRODUCT_CODE=${P_PRODUCT_CODE}"
                "-DAPP_UNIQUE_ID=${P_UNIQUE_ID}" "-DAPP_SYSTEM_MODE_EXT=${P_SYSTEM_MODE_EXT}"
        DEPENDS ${target} "${cia_smdh}" "${banner}" "${OMW3DS_PACKAGING_DIR}/app.rsf"
        COMMENT "Building CIA ${target}.cia (${P_SYSTEM_MODE_EXT})" VERBATIM)
    # Same exheader as the .cia, but bootable directly by emulators (Azahar honours its memory mode),
    # so emulator runs exercise exactly what the .cia requests.
    set(cxi "${CMAKE_CURRENT_BINARY_DIR}/${target}.cxi")
    add_custom_command(OUTPUT "${cxi}"
        COMMAND "${OMW3DS_MAKEROM}" -f ncch -o "${cxi}" -target t -exefslogo
                -elf "$<TARGET_FILE:${target}>" -rsf "${OMW3DS_PACKAGING_DIR}/app.rsf"
                -icon "${cia_smdh}" -banner "${banner}"
                "-DAPP_TITLE=${P_TITLE}" "-DAPP_PRODUCT_CODE=${P_PRODUCT_CODE}"
                "-DAPP_UNIQUE_ID=${P_UNIQUE_ID}" "-DAPP_SYSTEM_MODE_EXT=${P_SYSTEM_MODE_EXT}"
        DEPENDS ${target} "${cia_smdh}" "${banner}" "${OMW3DS_PACKAGING_DIR}/app.rsf"
        COMMENT "Building CXI ${target}.cxi (emulator twin of the .cia)" VERBATIM)
    add_custom_target(${target}_cia ALL DEPENDS "${cia}" "${cxi}")
endfunction()
