#include <stdlib.h>
#include <stdio.h>

#define SOKOL_METAL
#include "sokol_app.h"

#include "../lib/onedraw.h"

struct onedraw* renderer;

// ---------------------------------------------------------------------------------------------------------------------------
void init(void)
{
    renderer = od_init( &(onedraw_def)
    {
        .allow_screenshot = true,
        .metal_device = (void*)sapp_metal_get_device(),
        .preallocated_buffer = malloc(od_min_memory_size()),
        .viewport_width = (uint32_t) sapp_width(),
        .viewport_height = (uint32_t) sapp_height()
    });
}

// ---------------------------------------------------------------------------------------------------------------------------
void frame(void)
{
    od_begin_frame(renderer);
    od_draw_text(renderer, 0, 0, "Hello world!", 0xffffffff);
    od_end_frame(renderer, (void*)sapp_metal_get_current_drawable());
}

// ---------------------------------------------------------------------------------------------------------------------------
void cleanup(void)
{
    od_terminate(renderer);
}

// ---------------------------------------------------------------------------------------------------------------------------
sapp_desc sokol_main(int argc, char* argv[])
{
    (void)(argc);
    (void)(argv);

    return (sapp_desc) 
    {
        .width = 1280,
        .height = 720,
        .init_cb = init,
        .frame_cb = frame,
        .cleanup_cb = cleanup
    };
}

