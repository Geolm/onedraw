cmake_minimum_required(VERSION 3.25)
project(onedraw)

if(CMAKE_GENERATOR STREQUAL "Xcode")
    set(CMAKE_MACOSX_BUNDLE TRUE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS_DEBUG "-g -O0 -DDEBUG -Wall -Wextra ")
set(CMAKE_C_FLAGS_RELEASE "-O3 -Wall -Wextra")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -Wall -Wextra")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DDEBUG -Wall -Wextra")

# --- Optionally trigger pre-build ---
if(NOT CMAKE_GENERATOR STREQUAL "Xcode")
    add_subdirectory(pre-build)
endif()

# --- Minimal ---
add_executable(minimal
    ./lib/onedraw.cpp
    ./tests/minimal.c
    ./tests/sokol_app.mm
)

if(TARGET build_lib)
    add_dependencies(minimal build_lib)
endif()

target_link_libraries(minimal
    "-framework Metal"
    "-framework MetalKit"
    "-framework AppKit"
    "-framework Foundation"
    "-framework QuartzCore"
)

# --- Test ---
add_executable(test
    ./lib/onedraw.cpp
    ./tests/test.c
    ./tests/sokol_app.mm
)

if(TARGET build_lib)
    add_dependencies(test build_lib)
endif()

target_link_libraries(test
    "-framework Metal"
    "-framework MetalKit"
    "-framework AppKit"
    "-framework Foundation"
    "-framework QuartzCore"
)
