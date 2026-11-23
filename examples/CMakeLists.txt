# --- Clay example ---
# A VS Code-like editor UI : clay computes the layout, onedraw draws it
add_executable(clay_example
    ../lib/onedraw.cpp
    ./clay.c
    ../tests/sokol_app.mm
)

target_include_directories(clay_example PRIVATE ../third_party)

if(TARGET build_lib)
    add_dependencies(clay_example build_lib)
endif()

target_link_libraries(clay_example
    "-framework Metal"
    "-framework MetalKit"
    "-framework AppKit"
    "-framework Foundation"
    "-framework QuartzCore"
)

# --- lean_ui example ---
# An immediate-mode UI : lean_ui manages the windows and widgets, onedraw draws them
add_executable(lean_ui_example
    ../lib/onedraw.cpp
    ../third_party/lean_ui.c
    ./lean_ui.c
    ../tests/sokol_app.mm
)

target_include_directories(lean_ui_example PRIVATE ../third_party)

if(TARGET build_lib)
    add_dependencies(lean_ui_example build_lib)
endif()

target_link_libraries(lean_ui_example
    "-framework Metal"
    "-framework MetalKit"
    "-framework AppKit"
    "-framework Foundation"
    "-framework QuartzCore"
)
