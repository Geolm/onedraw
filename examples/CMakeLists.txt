# --- lean_ui example ---
# An immediate-mode UI : lean_ui manages the windows and widgets, onedraw draws them
add_executable(lean_ui_example
    ../lib/onedraw.cpp
    ../third_party/lean_ui.c
    ../third_party/mjpegw.c
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
