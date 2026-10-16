#include <stdlib.h>
#include <stdio.h>

#define SOKOL_METAL
#include "sokol_app.h"

#include "../lib/onedraw.h"

struct onedraw* renderer;

void init(void)
{
    sapp_environment env = sapp_get_environment();

    renderer = od_init( &(onedraw_def)
    {
        .metal_device = (void*)env.metal.device,
        .preallocated_buffer = malloc(od_min_memory_size()),
        .viewport_width = (uint32_t) sapp_width(),
        .viewport_height = (uint32_t) sapp_height()
    });
}

void frame(void)
{
    if (renderer == NULL)
        return;
    od_begin_frame(renderer);
    od_draw_text(renderer, 0, 0, "Hello world!", 0xffffffff);

    sapp_swapchain sc = sapp_acquire_swapchain();
    od_end_frame(renderer, (void*)sc.metal.current_drawable);
}

void cleanup(void)
{
    if (renderer != NULL)
        od_terminate(renderer);
    free(renderer);
    renderer = NULL;
}

sapp_desc sokol_main(int argc, char* argv[])
{
    (void) argc;(void) argv;

    return (sapp_desc) 
    {
        .width = 1280,
        .height = 720,
        .init_cb = init,
        .frame_cb = frame,
        .cleanup_cb = cleanup
    };
}
