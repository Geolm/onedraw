cmake_minimum_required(VERSION 3.25)
project(prebuild)

add_executable(builder
    bc4_encoder.c
    bin2h.c
    builder.c
    shader_reader.c
    ../third_party/bc_packed.c
)

target_include_directories(builder PRIVATE ../third_party)

# the generated headers are a function of the shaders, the fonts and the stb versions :
# listing them as OUTPUTS (with the builder executable and every input as DEPENDS)
# re-runs the builder when any input is newer, even if the builder binary itself
# is up to date (a TARGET POST_BUILD command cannot track its inputs)
set(prebuild_inputs
    ${CMAKE_SOURCE_DIR}/src/shaders/binning.metal
    ${CMAKE_SOURCE_DIR}/src/shaders/rasterizer.metal
    ${CMAKE_SOURCE_DIR}/src/shaders/bc_decompress.metal
    ${CMAKE_SOURCE_DIR}/src/shaders/common.h
    ${CMAKE_SOURCE_DIR}/third_party/bc_packed.h
    ${CMAKE_SOURCE_DIR}/fonts/Satoshi-Regular.otf
    ${CMAKE_SOURCE_DIR}/fonts/Satoshi-Bold.otf
    ${CMAKE_SOURCE_DIR}/fonts/Satoshi-Italic.otf
    ${CMAKE_SOURCE_DIR}/fonts/JetBrainsMono-Regular.ttf
    ${CMAKE_SOURCE_DIR}/third_party/stb_truetype.h
    ${CMAKE_SOURCE_DIR}/third_party/stb_rect_pack.h
)

set(prebuild_outputs
    ${CMAKE_SOURCE_DIR}/lib/default_font.h
    ${CMAKE_SOURCE_DIR}/lib/default_font_atlas.h
    ${CMAKE_SOURCE_DIR}/lib/bc_decompress.h
    ${CMAKE_SOURCE_DIR}/lib/binning.h
    ${CMAKE_SOURCE_DIR}/lib/rasterization.h
    ${CMAKE_SOURCE_DIR}/lib/common.h
)

add_custom_command(
    OUTPUT ${prebuild_outputs}
    COMMAND builder
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    DEPENDS builder ${prebuild_inputs}
    COMMENT "Building library ..."
)


add_custom_target(build_lib ALL DEPENDS ${prebuild_outputs})
