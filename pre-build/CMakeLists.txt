cmake_minimum_required(VERSION 3.25)
project(prebuild)

add_executable(builder
    bc4_encoder.c
    bin2h.c
    builder.c
    shader_reader.c
)

target_include_directories(builder PRIVATE ../third_party)

# the generated headers are a function of the shaders, the fonts and the stb versions :
# tracking them as byproducts keeps them fresh even when the builder binary itself
# is up to date (a .metal or a font edit must not wait for a builder.c change)
add_custom_command(
    TARGET builder
    POST_BUILD
    COMMAND builder
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    DEPENDS
        ${CMAKE_SOURCE_DIR}/src/shaders/binning.metal
        ${CMAKE_SOURCE_DIR}/src/shaders/rasterizer.metal
        ${CMAKE_SOURCE_DIR}/src/shaders/common.h
        ${CMAKE_SOURCE_DIR}/fonts/Satoshi-Regular.otf
        ${CMAKE_SOURCE_DIR}/fonts/Satoshi-Bold.otf
        ${CMAKE_SOURCE_DIR}/fonts/Satoshi-Italic.otf
        ${CMAKE_SOURCE_DIR}/fonts/JetBrainsMono-Regular.ttf
        ${CMAKE_SOURCE_DIR}/third_party/stb_truetype.h
        ${CMAKE_SOURCE_DIR}/third_party/stb_rect_pack.h
    BYPRODUCTS
        ${CMAKE_SOURCE_DIR}/lib/default_font.h
        ${CMAKE_SOURCE_DIR}/lib/default_font_atlas.h
        ${CMAKE_SOURCE_DIR}/lib/binning.h
        ${CMAKE_SOURCE_DIR}/lib/rasterization.h
        ${CMAKE_SOURCE_DIR}/lib/common.h
    COMMENT "Building library ..."
)


add_custom_target(build_lib ALL DEPENDS builder)
