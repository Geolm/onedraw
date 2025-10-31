#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#define SOKOL_METAL
#include "sokol_app.h"

#include "../lib/onedraw.h"
struct onedraw* renderer;

#define FROM_HTML(html)   ((html&0xff)<<16) | ((html>>16)&0xff) | (html&0x00ff00) | 0xff000000
#define TEX_SIZE (256)

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

//-----------------------------------------------------------------------------------------------------------------------------
static inline uint32_t lerp_color(uint32_t a, uint32_t b, float t)
{
    int tt = (int)(t * 256.f);
    int oneminust = 256 - tt;

    uint32_t A = (((a >> 24) & 0xFF) * oneminust + ((b >> 24) & 0xFF) * tt) >> 8;
    uint32_t B = (((a >> 16) & 0xFF) * oneminust + ((b >> 16) & 0xFF) * tt) >> 8;
    uint32_t G = (((a >> 8)  & 0xFF) * oneminust + ((b >> 8)  & 0xFF) * tt) >> 8;
    uint32_t R = (((a >> 0)  & 0xFF) * oneminust + ((b >> 0)  & 0xFF) * tt) >> 8;

    return (A << 24) | (B << 16) | (G << 8) | R;
}

// ---------------------------------------------------------------------------------------------------------------------------
void make_checker(uint32_t* p, uint32_t w, uint32_t h, uint32_t a, uint32_t b)
{
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            *p++ = (((x >> 5) ^ (y >> 5)) & 1) ? a : b;
}

// ---------------------------------------------------------------------------------------------------------------------------
void make_rings(uint32_t* p, uint32_t w, uint32_t h, uint32_t a, uint32_t b)
{
    float cx = (float)w * 0.5f, cy = (float)h * 0.5f;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) 
        {
            float dx = (float)x - cx, dy = (float)y - cy;
            uint32_t ring = (uint32_t)(sqrtf(dx * dx + dy * dy)) / 16;
            *p++ = (ring & 1) ? a : b;
        }
}

// ---------------------------------------------------------------------------------------------------------------------------
void make_hgradient(uint32_t* p, uint32_t w, uint32_t h, uint32_t a, uint32_t b)
{
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
        {
            float t = (float)x / (float)(w - 1);
            *p++ = lerp_color(a, b, t);
        }
}

// ---------------------------------------------------------------------------------------------------------------------------
void make_radial(uint32_t* p, uint32_t w, uint32_t h, uint32_t a, uint32_t b)
{
    float cx = (float)w * 0.5f;
    float cy = (float)h * 0.5f;
    float maxd = sqrtf(cx*cx + cy*cy);

    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) 
        {
            float dx = (float)x - cx;
            float dy = (float)y - cy;
            float t = sqrtf(dx*dx + dy*dy) / maxd;
            if (t > 1.f) t = 1.f;
            *p++ = lerp_color(a, b, t);
        }
}

// ---------------------------------------------------------------------------------------------------------------------------
void fill_texture_array(void)
{
    uint32_t* pixel_data = malloc(TEX_SIZE * TEX_SIZE);

    make_checker(pixel_data, TEX_SIZE, TEX_SIZE, miya_green, miya_yellow);
    od_upload_slice(renderer, pixel_data, 0);

    make_hgradient(pixel_data, TEX_SIZE, TEX_SIZE, miya_dark_blue, miya_light_blue);
    od_upload_slice(renderer, pixel_data, 1);

    make_rings(pixel_data, TEX_SIZE, TEX_SIZE, miya_brown, miya_pink);
    od_upload_slice(renderer, pixel_data, 2);

    make_radial(pixel_data, TEX_SIZE, TEX_SIZE, miya_black, miya_light_green);
    od_upload_slice(renderer, pixel_data, 3);

    free(pixel_data);
}


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
            .width = TEX_SIZE,
            .height = TEX_SIZE,
            .num_slices = 4
        }
    });

    od_set_clear_color(renderer, miya_white);
    fill_texture_array();
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
    (void) argc;
    (void) argv;
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
