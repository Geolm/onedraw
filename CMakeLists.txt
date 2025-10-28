cmake_minimum_required(VERSION 3.25)

if(CMAKE_GENERATOR STREQUAL "Xcode")
    set(CMAKE_MACOSX_BUNDLE TRUE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS_DEBUG "-g -O0 -DDEBUG -Wall -Wextra ")
set(CMAKE_C_FLAGS_RELEASE "-O3 -Wall -Wextra")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -Wall -Wextra")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DDEBUG -Wall -Wextra")

project(sdf2d)

# --- Header builder ---

add_executable(builder
./pre-build/bc4_encoder.c
./pre-build/bin2h.c
./pre-build/builder.c
./pre-build/shader_reader.c
)

add_custom_command(
    TARGET builder
    POST_BUILD
    COMMAND builder
    COMMENT "Building library ..."
)

add_custom_target(build_lib ALL DEPENDS builder)

# --- Benchmark ---

add_executable(benchmark
./lib/onedraw.cpp
./tests/test.c
./tests/sokol_app.mm
)

add_dependencies(benchmark build_lib)

target_link_libraries(benchmark
        "-framework Metal"
        "-framework MetalKit"
        "-framework AppKit"
        "-framework Foundation"
        "-framework QuartzCore")