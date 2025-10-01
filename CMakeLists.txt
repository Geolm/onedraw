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
./pre-build/builder.c
)

add_custom_command(
    TARGET builder
    POST_BUILD
    COMMAND builder
    COMMENT "Building one-header lib..."
)

add_custom_target(build_lib ALL DEPENDS builder)

# --- Benchmark ---

find_package(glfw3 3.4 REQUIRED)

add_executable(benchmark
./tests/benchmark.c
)

add_dependencies(benchmark build_lib)

target_link_libraries(benchmark glfw)
target_link_libraries(benchmark
        "-framework Metal"
        "-framework MetalKit"
        "-framework AppKit"
        "-framework Foundation"
        "-framework QuartzCore")