add_executable(testapp
    source/main.cpp
    source/renderer.cpp
    source/probes.cpp
)

ctr_add_shader_library(testapp_shaders OUTPUT lit.shbin source/lit.v.pica)
dkp_add_embedded_binary_library(testapp_shaders_bin testapp_shaders)

target_compile_options(testapp PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(testapp PRIVATE platform3ds testapp_shaders_bin citro3d ctru m)

dkp_target_generate_symbol_list(testapp)

omw3ds_package(testapp
    TITLE "OpenMW3DS testapp"
    DESCRIPTION "M0 platform probes"
    AUTHOR "OpenMW 3DS port"
    PRODUCT_CODE "CTR-P-OMWT"
    UNIQUE_ID 0xF6D30
    SYSTEM_MODE_EXT 178MB
)
