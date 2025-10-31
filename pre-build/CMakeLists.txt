cmake_minimum_required(VERSION 3.25)
project(prebuild)

add_executable(builder
    bc4_encoder.c
    bin2h.c
    builder.c
    shader_reader.c
)

add_custom_command(
    TARGET builder
    POST_BUILD
    COMMAND builder
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Building library ..."
)


add_custom_target(build_lib ALL DEPENDS builder)
