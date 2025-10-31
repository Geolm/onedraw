#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#define SOKOL_METAL
#include "sokol_app.h"

#include "../lib/onedraw.h"
struct onedraw* renderer;

#define FROM_HTML(html)   ((html&0xff)<<16) | ((html>>16)&0xff) | (html&0x00ff00) | 0xff000000
#define TEX_SIZE (256)
#define PI_4 (0.78539816f)

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
    uint32_t* pixel_data = malloc(TEX_SIZE * TEX_SIZE * sizeof(uint32_t));

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
        .atlas = 
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
void slot(uint32_t index, float* cx, float* cy, float* radius)
{
    float step_x = sapp_widthf() / 6.f;
    float step_y = sapp_heightf() / 3.375f;

    *cx = (index%6) * step_x + step_x * .5f;
    *cy = (index/6) * step_y + step_y * .5f;
    *radius = fminf(step_x, step_y) * .4f;
}

// ---------------------------------------------------------------------------------------------------------------------------
void frame(void)
{
    float cx, cy, radius;

    od_begin_frame(renderer);

    slot(0, &cx, &cy, &radius);
    od_draw_disc(renderer, cx, cy, radius, miya_blue);
    od_draw_text(renderer, cx-radius, cy-radius*1.25f, "odd_draw_text", miya_brown);

    slot(1, &cx, &cy, &radius);
    od_draw_ring(renderer, cx, cy, radius, radius * .1f, miya_green);
    od_draw_text(renderer, cx-radius, cy-radius*1.25f, "od_draw_ring", miya_brown);

    slot(2, &cx, &cy, &radius);
    od_draw_box(renderer, cx - radius, cy - radius*.5f, cx + radius, cy + radius*.5f, radius * 0.05f, miya_grey);
    od_draw_text(renderer, cx-radius, cy-radius*1.25f, "od_draw_box", miya_brown);

    slot(3, &cx, &cy, &radius);
    od_draw_blurred_box(renderer, cx, cy, radius*.25f, radius*.5f, radius * 0.1f, miya_black);
    od_draw_text(renderer, cx-radius, cy-radius*1.25f, "od_draw_blurred_box", miya_brown);

    slot(4, &cx, &cy, &radius);
    od_draw_oriented_rect(renderer, cx - cosf(PI_4) * radius, cy - sinf(PI_4) * radius, cx + cosf(PI_4) * radius, cy + sinf(PI_4) * radius,
                          radius * 0.4f, 0.f, radius * 0.1f, miya_pale_blue);
    od_draw_text(renderer, cx-radius, cy-radius*1.25f, "od_draw_oriented_rect", miya_brown);

    slot(5, &cx, &cy, &radius);
    od_draw_oriented_box(renderer, cx + cosf(PI_4) * radius, cy - sinf(PI_4) * radius, cx - cosf(PI_4) * radius, cy + sinf(PI_4) * radius,
                         radius * 0.5f, radius * 0.05f, miya_red);
    od_draw_text(renderer, cx-radius, cy-radius*1.25f, "od_draw_oriented_box", miya_brown);

    slot(6, &cx, &cy, &radius);
    od_draw_triangle(renderer, (float[]){cx, cy-radius, cx - cosf(PI_4) * radius, cy + sinf(PI_4) * radius,
                     cx + cosf(PI_4) * radius, cy +sinf(PI_4) * radius}, radius * 0.1f, miya_dark_green);
    od_draw_text(renderer, cx-radius, cy-radius*1.25f, "od_draw_triangle", miya_brown);

    slot(7, &cx, &cy, &radius);
    od_draw_triangle_ring(renderer, (float[]){cx, cy+radius, cx - cosf(PI_4) * radius, cy - sinf(PI_4) * radius,
                          cx + cosf(PI_4) * radius, cy - sinf(PI_4) * radius}, 0.f, radius * 0.1f, miya_dark_grey);
    od_draw_text(renderer, cx-radius, cy-radius*1.25f, "od_draw_triangle_ring", miya_brown);

    slot(8, &cx, &cy, &radius);
    od_draw_ellipse(renderer, cx + cosf(PI_4) * radius, cy - sinf(PI_4) * radius, cx - cosf(PI_4) * radius, cy + sinf(PI_4) * radius,
                    radius, miya_yellow);
    od_draw_text(renderer, cx-radius, cy-radius*1.25f, "od_draw_ellipse", miya_brown);

    slot(9, &cx, &cy, &radius);
    od_draw_ellipse_ring(renderer, cx + cosf(PI_4) * radius, cy - sinf(PI_4) * radius, cx - cosf(PI_4) * radius, cy + sinf(PI_4) * radius,
                         radius, radius * 0.1f, miya_light_grey);
    od_draw_text(renderer, cx-radius, cy-radius*1.25f, "od_draw_ellipse_ring", miya_brown);

    slot(10, &cx, &cy, &radius);
    od_draw_sector(renderer, cx, cy, radius, 0.123f, PI_4, miya_pink);
    od_draw_text(renderer, cx-radius, cy-radius*1.25f, "od_draw_sector", miya_brown);

    slot(11, &cx, &cy, &radius);
    od_draw_sector_ring(renderer, cx, cy, radius, -0.1234f, -PI_4*2.f, radius * 0.1f, miya_dark_blue);
    od_draw_text(renderer, cx-radius, cy-radius*1.25f, "od_draw_sector_ring", miya_brown);

    slot(12, &cx, &cy, &radius);
    od_draw_arc(renderer, cx, cy, cosf(PI_4), sinf(PI_4), PI_4*0.66f, radius, radius * 0.1f, miya_red);
    od_draw_text(renderer, cx-radius, cy-radius*1.25f, "od_draw_arc", miya_brown);

    slot(13, &cx, &cy, &radius);
    od_draw_text(renderer, cx-radius, cy-radius, "Some text\nABCDEFGHILMNOPQRSTUVWYZ\n1234567890!@#$%?&*()\nSphinx of black quartz, judge my vow.\n"
                 "!\"#$%&'()*+,-./0123456789:;<=>?@\n[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~", miya_black);
    od_draw_text(renderer, cx-radius, cy-radius*1.25f, "od_draw_text", miya_brown);

    slot(14, &cx, &cy, &radius);
    od_begin_group(renderer, true, radius * 0.25f, radius * 0.05f);
    od_draw_disc(renderer, cx, cy, radius*0.25f, miya_light_green);
    od_draw_disc(renderer, cx + cosf(PI_4) * radius * .5f, cy - sinf(PI_4) * radius * .5f, radius*0.25f, miya_light_green);
    od_end_group(renderer, miya_brown);
    od_draw_text(renderer, cx-radius, cy-radius*1.25f, "od_begin_group", miya_brown);

    slot(15, &cx, &cy, &radius);
    od_draw_quad(renderer, cx-radius, cy-radius, cx, cy, (od_quad_uv){0.f, 0.f, 1.f, 1.f}, 0, 0x7fffffff);
    od_draw_quad(renderer, cx, cy-radius, cx+radius, cy, (od_quad_uv){0.f, 0.f, 1.f, 1.f}, 1, 0xffffffff);
    od_draw_quad(renderer, cx-radius, cy, cx, cy+radius, (od_quad_uv){0.f, 0.f, 1.f, 1.f}, 2, 0xffffffff);
    od_draw_quad(renderer, cx, cy, cx+radius, cy+radius, (od_quad_uv){0.f, 0.f, 1.f, 1.f}, 3, 0xffffffff);
    od_draw_text(renderer, cx-radius, cy-radius*1.25f, "od_draw_quad", miya_brown);

    slot(16, &cx, &cy, &radius);
    od_draw_oriented_quad(renderer, cx, cy, radius, radius*.5f, PI_4 * 0.75f, (od_quad_uv){0.f, 0.f, 1.f, 0.5f}, 2, 0xffffffff);
    od_draw_text(renderer, cx-radius, cy-radius*1.25f, "od_draw_oriented_quad", miya_brown);

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
