#include "../lib/onedraw.h"

// The onedraw draw primitives referenced by scene.c (the od_* stub set shared with parse_check)
#include "parse_check_stubs.c"

// The window shim (window.mm) is AppKit ; the headless checks drive it through this stub
int shot_stub_last_w = 0;
int shot_stub_last_h = 0;
int shot_stub_calls = 0;

void mcp_set_window_points(int w, int h)
{
    shot_stub_last_w = w;
    shot_stub_last_h = h;
    shot_stub_calls++;
}
