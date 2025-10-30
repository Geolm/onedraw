#include <stdlib.h>
#include <stdio.h>

#define SOKOL_METAL
#include "sokol_app.h"

#include "../lib/onedraw.h"

#define FROM_HTML(html)   ((html&0xff)<<16) | ((html>>16)&0xff) | (html&0x00ff00) | 0xff000000

// https://lospec.com/palette-list/miyazaki-16
static const uint32_t miya_black = FROM_HTML(0x232228);
static const uint32_t miya_dark_blue = FROM_HTML(0x284261);
static const uint32_t miya_dark_grey = FROM_HTML(0x5f5854);
static const uint32_t miya_grey = FROM_HTML(0x878573);
static const uint32_t miya_light_grey = FROM_HTML(0xb8b095);
static const uint32_t miya_pale_blue = FROM_HTML(0xc3d5c7);
static const uint32_t miya_white = FROM_HTML(0xebecdc);
static const uint32_t miya_blue = FROM_HTML(0x2485a6);
static const uint32_t miya_light_blue = FROM_HTML(0x54bad2);
static const uint32_t miya_brown = FROM_HTML(0x754d45);
static const uint32_t miya_red = FROM_HTML(0xc65046);
static const uint32_t miya_pink = FROM_HTML(0xe6928a);
static const uint32_t miya_dark_green = FROM_HTML(0x1e7453);
static const uint32_t miya_green = FROM_HTML(0x55a058);
static const uint32_t miya_light_green = FROM_HTML(0xa1bf41);
static const uint32_t miya_yellow = FROM_HTML(0xe3c054);

struct onedraw* renderer;

// ---------------------------------------------------------------------------------------------------------------------------
void custom_log(const char* string)
{
    fprintf(stdout, "%s\n", string);
}

// ---------------------------------------------------------------------------------------------------------------------------
void init(void)
{
    renderer = od_init( &(onedraw_def)
    {
        .metal_device = (void*)sapp_metal_get_device(),
        .preallocated_buffer = malloc(od_min_memory_size()),
        .viewport_width = (uint32_t) sapp_width(),
        .viewport_height = (uint32_t) sapp_height(),
        .log_func = custom_log,
        .srgb_backbuffer = false,
        .texture_array = 
        {
            .width = 256,
            .height = 256,
            .num_slices = 4
        }
    });

    od_set_clear_color(renderer, miya_white);
}

// ---------------------------------------------------------------------------------------------------------------------------
void frame(void)
{
    od_begin_frame(renderer);
    od_end_frame(renderer, (void*)sapp_metal_get_current_drawable());
}

// ---------------------------------------------------------------------------------------------------------------------------
void cleanup(void)
{
    od_terminate(renderer);
    free(renderer);
}

// ---------------------------------------------------------------------------------------------------------------------------
sapp_desc sokol_main(int argc, char* argv[])
{
    return (sapp_desc) 
    {
        .width = 1280,
        .height = 720,
        .init_cb = init,
        .frame_cb = frame,
        .cleanup_cb = cleanup,
        .high_dpi = true
    };
}
