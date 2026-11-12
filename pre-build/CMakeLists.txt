cmake_minimum_required(VERSION 3.25)
project(prebuild)

option(ONEDRAW_SHADER_DEBUG_INFO "Compile the shaders with -g -frecord-sources" OFF)

# the minimum macOS the precompiled shaders must run on : it is baked into the Metal library's target
# tag and is the runtime floor of the library (the consuming app's own deployment target does not
# affect it) ; the shader sources currently compile down to 15.0 (AIR v27) if a wider floor is wanted
set(ONEDRAW_DEPLOY_TARGET "26.0" CACHE STRING "Minimum macOS version for the precompiled shaders")

add_executable(builder
    bc4_encoder.c
    bin2h.c
    builder.c
    ../third_party/bc_packed.c
)

target_include_directories(builder PRIVATE ../third_party)

set(shader_dir ${CMAKE_CURRENT_BINARY_DIR}/shaders)
set(shader_flags_stamp ${CMAKE_CURRENT_BINARY_DIR}/shader_flags.txt)
file(CONFIGURE OUTPUT ${shader_flags_stamp} CONTENT "${ONEDRAW_SHADER_DEBUG_INFO}:${ONEDRAW_DEPLOY_TARGET}" @ONLY)

set(shader_flags -mmacosx-version-min=${ONEDRAW_DEPLOY_TARGET})
if(ONEDRAW_SHADER_DEBUG_INFO)
    list(APPEND shader_flags -g -frecord-sources)
endif()

set(shader_air_files)
foreach(shader binning rasterizer bc_decompress)
    set(shader_source ${CMAKE_SOURCE_DIR}/src/shaders/${shader}.metal)
    set(shader_air ${shader_dir}/${shader}.air)

    add_custom_command(
        OUTPUT ${shader_air}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
        COMMAND xcrun -sdk macosx metal ${shader_flags} -c ${shader_source} -o ${shader_air}
        DEPENDS ${shader_source} ${CMAKE_SOURCE_DIR}/src/shaders/common.h ${shader_flags_stamp}
        COMMENT "Compiling ${shader}.metal"
    )

    list(APPEND shader_air_files ${shader_air})
endforeach()

set(shader_library ${shader_dir}/shaders.metallib)

add_custom_command(
    OUTPUT ${shader_library}
    COMMAND xcrun -sdk macosx metallib ${shader_air_files} -o ${shader_library}
    DEPENDS ${shader_air_files}
    COMMENT "Linking shaders.metallib"
)

# the generated headers are a function of the shader library, the fonts and the stb versions :
# listing them as OUTPUTS (with the builder executable and every input as DEPENDS)
# re-runs the builder when any input is newer, even if the builder binary itself
# is up to date (a TARGET POST_BUILD command cannot track its inputs)
set(prebuild_inputs
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
    ${CMAKE_SOURCE_DIR}/lib/shaders.h
    ${CMAKE_SOURCE_DIR}/lib/common.h
)

add_custom_command(
    OUTPUT ${prebuild_outputs}
    COMMAND builder ${shader_library} ${shader_flags}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    DEPENDS builder ${prebuild_inputs} ${shader_library}
    COMMENT "Building library ..."
)


add_custom_target(build_lib ALL DEPENDS ${prebuild_outputs})
