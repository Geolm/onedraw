#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdbool.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define FLAG_IMPLEMENTATION
#include "flag.h"

#define SOKOL_METAL
#include "sokol_app.h"

#include "../lib/onedraw.h"
struct onedraw* renderer;
bool culling_debug = false;

// command line options, parsed in sokol_main
struct test_options
{
    bool autoexit;     // quit after 100 frames
    bool compare;      // exit non-zero if the screenshot differs from the reference
    bool update_refs;  // rewrite the matching reference image from the current capture
    bool cache;        // enable the tile cache
};

struct test_options options = {false, false, false, false};

#define FROM_HTML(html)   ((html&0xff)<<16) | ((html>>16)&0xff) | (html&0x00ff00) | 0xff000000
#define TEX_SIZE (256)
#define REFERENCE_PSNR_THRESHOLD_DB (60.0f)

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
    if (renderer == NULL)
        return;

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
static inline float iq_random_float(int* seed)
{
    union
    {
        float fres;
        unsigned int ires;
    } float2int;

    *seed *= 16807;

    float2int.ires = ((((unsigned int)*seed)>>9 ) | 0x3f800000);
    return float2int.fres - 1.0f;
}

//----------------------------------------------------------------------------------------------------------------------------
static void bgra_to_rgba(uint8_t *pixels, uint32_t width, uint32_t height)
{
    uint32_t count = width * height;

    for (uint32_t i = 0; i < count; ++i)
    {
        uint8_t b = pixels[i * 4 + 0];
        pixels[i * 4 + 0] = pixels[i * 4 + 2];
        pixels[i * 4 + 2] = b;
    }
}

// ---------------------------------------------------------------------------------------------------------------------------
static const char* reference_name(uint32_t width, uint32_t height)
{
    return (width == 1280 && height == 720) ? "reference.png" : "reference_highdpi.png";
}

// Regression check: compare the captured frame against the committed reference image.
// Identical images are a clean pass; otherwise the PSNR (all RGBA channels) must stay
// above the threshold. Only --compare makes a mismatch fail the run; --update-refs
// rewrites the matching reference from the current capture.
static void compare_with_reference(const uint8_t* pixels, uint32_t width, uint32_t height)
{
    const char* name = reference_name(width, height);

    uint32_t ref_width = 0, ref_height = 0, ref_channels = 0;
    uint8_t* reference = stbi_load(name, (int*)&ref_width, (int*)&ref_height, (int*)&ref_channels, 4);

    if (reference == NULL)
    {
        if (options.update_refs)
        {
            stbi_write_png(name, (int)width, (int)height, 4, pixels, (int)width * 4);
            fprintf(stdout, "reference : %s created from current capture\n", name);
        }
        else
            fprintf(stderr, "reference : %s not found, comparison skipped\n", name);
        return;
    }

    if (ref_width != width || ref_height != height)
    {
        fprintf(stderr, "reference : %s is %ux%u, screenshot is %ux%u, comparison skipped\n",
                name, ref_width, ref_height, width, height);
        stbi_image_free(reference);
        return;
    }

    double ssd = 0.0;
    uint32_t diff_pixels = 0, max_delta = 0;
    uint32_t pixel_count = width * height;

    for (uint32_t p = 0; p < pixel_count; ++p)
    {
        uint32_t c;
        bool pixel_differs = false;

        for (c = 0; c < 4; ++c)
        {
            int a = pixels[(p * 4u) + c];
            int b = reference[(p * 4u) + c];
            int delta = (a > b) ? (a - b) : (b - a);

            if (delta != 0)
            {
                pixel_differs = true;
                ssd += (double)delta * (double)delta;
            }

            if (delta > (int)max_delta)
                max_delta = (uint32_t)delta;
        }

        if (pixel_differs)
            diff_pixels++;
    }

    stbi_image_free(reference);

    if (ssd == 0.0)
    {
        fprintf(stdout, "reference : %s identical\n", name);
        return;
    }

    float psnr = (float)(10.0 * log10((255.0 * 255.0) / (ssd / (double)pixel_count / 4.0)));
    if (psnr >= REFERENCE_PSNR_THRESHOLD_DB)
    {
        fprintf(stdout, "reference : %s psnr %.1f dB (threshold %.1f) - ok\n", name, psnr, REFERENCE_PSNR_THRESHOLD_DB);
    }
    else
    {
        fprintf(stderr, "reference : %s psnr %.1f dB (threshold %.1f) - MISMATCH (%u/%u px differ, max delta %u)\n",
                name, psnr, REFERENCE_PSNR_THRESHOLD_DB, diff_pixels, pixel_count, max_delta);
        if (options.compare)
            exit(1);
    }

    if (options.update_refs)
    {
        stbi_write_png(name, (int)width, (int)height, 4, pixels, (int)width * 4);
        fprintf(stdout, "reference : %s updated from current capture\n", name);
    }
}

// ---------------------------------------------------------------------------------------------------------------------------
void custom_log(const char* string)
{
    fprintf(stdout, "%s\n", string);
}

// ---------------------------------------------------------------------------------------------------------------------------
void init(void)
{
    sapp_environment env = sapp_get_environment();

    renderer = od_init( &(onedraw_def)
    {
        .metal_device = (void*)env.metal.device,
        .preallocated_buffer = malloc(od_min_memory_size()),
        .viewport_width = (uint32_t) sapp_width(),
        .viewport_height = (uint32_t) sapp_height(),
        .log_func = custom_log,
        .srgb_backbuffer = false,
        .allow_screenshot = true,
        .high_dpi = sapp_high_dpi(),
        .cache = options.cache,
        .atlas = 
        {
            .width = TEX_SIZE,
            .height = TEX_SIZE,
            .num_slices = 4
        }
    });

    if (renderer == NULL)
        return;

    od_set_clear_color(renderer, miya_white);
    fill_texture_array();
}

// ---------------------------------------------------------------------------------------------------------------------------
void slot(uint32_t index, float* cx, float* cy, float* radius)
{
    float step_x = sapp_widthf() / 8.f;
    float step_y = sapp_heightf() / 3.375f;

    *cx = (index%8) * step_x + step_x * .5f;
    *cy = (index/8) * step_y + step_y * .5f;
    *radius = fminf(step_x, step_y) * .4f;
}

const float angle = 0.78539816f;
float gpu_time_ms = 0.f;
uint32_t num_frames = 0;
uint32_t frame_count = 0;

// ---------------------------------------------------------------------------------------------------------------------------
void frame(void)
{
    if (renderer == NULL)
        return;

    frame_count++;

    float cx, cy, radius;
    char string[256];

    od_begin_frame(renderer);

    slot(0, &cx, &cy, &radius);
    od_draw_disc_gradient(renderer, cx, cy, radius, miya_dark_blue, miya_light_blue);
    od_draw_text(renderer, cx-radius, cy-radius*1.25f, "disc_gradient", miya_brown);

    slot(1, &cx, &cy, &radius);
    od_draw_ring(renderer, cx, cy, radius, radius * .1f, miya_green);
    od_draw_text(renderer, cx-radius, cy-radius*1.25f, "ring", miya_brown);

    slot(2, &cx, &cy, &radius);
    od_draw_box(renderer, cx - radius, cy - radius*.5f, cx + radius, cy + radius*.5f, radius * 0.05f, miya_grey);
    od_draw_text(renderer, cx-radius, cy-radius*1.25f, "box", miya_brown);

    slot(3, &cx, &cy, &radius);
    od_draw_blurred_box(renderer, cx, cy, radius*.5f, radius, radius * 0.1f, miya_black);
    od_draw_text(renderer, cx-radius, cy-radius*1.25f, "blurred_box", miya_brown);

    slot(4, &cx, &cy, &radius);
    od_draw_oriented_rect(renderer, cx - cosf(angle) * radius, cy - sinf(angle) * radius, cx + cosf(angle) * radius, cy + sinf(angle) * radius,
                          radius * 0.4f, 0.f, radius * 0.1f, miya_pale_blue);
    od_draw_text(renderer, cx-radius, cy-radius*1.25f, "oriented_rect", miya_brown);

    slot(5, &cx, &cy, &radius);
    od_draw_oriented_box(renderer, cx + cosf(angle) * radius, cy - sinf(angle) * radius, cx - cosf(angle) * radius, cy + sinf(angle) * radius,
                         radius * 0.5f, radius * 0.05f, miya_red);
    od_draw_text(renderer, cx-radius, cy-radius*1.25f, "oriented_box", miya_brown);

    slot(6, &cx, &cy, &radius);
    od_draw_triangle(renderer, (float[]){cx, cy, cx - cosf(angle) * radius, cy + sinf(angle) * radius,
                     cx + cosf(angle) * radius, cy +sinf(angle) * radius}, radius * 0.1f, miya_dark_green);
    od_draw_text(renderer, cx-radius, cy-radius*1.25f, "triangle", miya_brown);

    slot(7, &cx, &cy, &radius);
    od_draw_triangle_ring(renderer, (float[]){cx, cy, cx - cosf(angle) * radius, cy - sinf(angle) * radius,
                          cx + cosf(angle) * radius, cy - sinf(angle) * radius}, 0.f, radius * 0.1f, miya_dark_grey);
    od_draw_text(renderer, cx-radius, cy-radius*1.25f, "triangle_ring", miya_brown);

    slot(8, &cx, &cy, &radius);
    od_draw_ellipse(renderer, cx + cosf(angle) * radius, cy - sinf(angle) * radius, cx - cosf(angle) * radius, cy + sinf(angle) * radius,
                    radius, miya_yellow);
    od_draw_text(renderer, cx-radius, cy-radius*1.25f, "ellipse", miya_brown);

    slot(9, &cx, &cy, &radius);
    od_draw_ellipse_ring(renderer, cx + cosf(angle) * radius, cy - sinf(angle) * radius, cx - cosf(angle) * radius, cy + sinf(angle) * radius,
                         radius, radius * 0.1f, miya_light_grey);
    od_draw_text(renderer, cx-radius, cy-radius*1.25f, "ellipse_ring", miya_brown);

    slot(10, &cx, &cy, &radius);
    od_draw_sector(renderer, cx, cy, radius, angle, 0.78539816f, miya_pink);
    od_draw_text(renderer, cx-radius, cy-radius*1.25f, "sector", miya_brown);

    slot(11, &cx, &cy, &radius);
    od_draw_sector_ring(renderer, cx, cy, radius, -angle, 0.78539816f, radius * 0.1f, miya_dark_blue);
    od_draw_text(renderer, cx-radius, cy-radius*1.25f, "sector_ring", miya_brown);

    slot(12, &cx, &cy, &radius);
    od_draw_arc(renderer, cx, cy, cosf(angle), sinf(angle), 0.78539816f, radius, radius * 0.1f, miya_red);
    od_draw_text(renderer, cx-radius, cy-radius*1.25f, "arc", miya_brown);

    slot(13, &cx, &cy, &radius);
    od_set_cliprect(renderer, cx - radius, cy - radius, cx + radius, cy + radius);
    od_draw_text(renderer, cx-radius, cy-radius, "Some text\nABCDEFGHILMNO\nPQRSTUVWYZ\n1234567890!@#$%?&*()\nSphinx of black\n quartz, judge my vow.\n"
                 "!\"#$%&'()*+,-./01234\n56789:;<=>?@\n[\\]^_`abcdefghijklmnop\nqrstuvwxyz{|}~", miya_black);
    od_set_cliprect(renderer, 0.f, 0.f, UINT16_MAX, UINT16_MAX);
    od_draw_text(renderer, cx-radius, cy-radius*1.25f, "text", miya_brown);

    slot(14, &cx, &cy, &radius);
    od_begin_group(renderer, true, radius * 0.25f, radius * 0.05f);
    od_draw_disc(renderer, cx, cy, radius*0.25f, miya_light_green);
    od_draw_disc(renderer, cx + cosf(angle) * radius * .5f, cy - sinf(angle) * radius * .5f, radius*0.25f, miya_light_green);
    od_draw_box(renderer, cx-radius*.7f, cy-radius*.7f, cx-radius*.3f, cy+radius*.7f, 0.f, miya_yellow);
    od_end_group(renderer, miya_brown);
    od_draw_text(renderer, cx-radius, cy-radius*1.25f, "smoothmin", miya_brown);

    slot(15, &cx, &cy, &radius);
    od_set_cliprect_rounded(renderer, cx - radius*.5f, cy - radius*.5f, cx + radius*.5f, cy + radius*.5f, radius*.25f);
    od_draw_quad(renderer, cx-radius, cy-radius, cx, cy, (od_quad_uv){0.f, 0.f, 1.f, 1.f}, 0, 0x7fffffff);
    od_draw_quad(renderer, cx, cy-radius, cx+radius, cy, (od_quad_uv){0.f, 0.f, 1.f, 1.f}, 1, 0x7fffffff);
    od_draw_quad(renderer, cx-radius, cy, cx, cy+radius, (od_quad_uv){0.f, 0.f, 1.f, 1.f}, 2, 0x7fffffff);
    od_draw_quad(renderer, cx, cy, cx+radius, cy+radius, (od_quad_uv){0.f, 0.f, 1.f, 1.f}, 3, 0x7fffffff);
    od_set_cliprect(renderer, 0.f, 0.f, UINT16_MAX, UINT16_MAX);
    od_draw_text(renderer, cx-radius, cy-radius*1.25f, "quad\nrounded clip", miya_brown);

    slot(16, &cx, &cy, &radius);
    od_draw_oriented_quad(renderer, cx, cy, radius, radius*.5f, angle * 0.75f, (od_quad_uv){0.f, 0.f, 1.f, 0.5f}, 2, 0x7fffffff);
    od_draw_text(renderer, cx-radius, cy-radius*1.25f, "oriented_quad", miya_brown);

    slot(17, &cx, &cy, &radius);
    float quadratic_ctrl_pts[] = {cx, cy-radius*.8f, cx-radius, cy+radius*0.8f, cx, cy+radius};
    uint32_t num_capsules = od_draw_quadratic_bezier(renderer, quadratic_ctrl_pts, 20.f, miya_red);
    snprintf(string, 256, "%u capsules", num_capsules);
    for(uint32_t i=0; i<3; i++)
        od_draw_disc(renderer, quadratic_ctrl_pts[i*2], quadratic_ctrl_pts[i*2+1], 10.f, miya_yellow);
    od_draw_text(renderer, cx-radius, cy-radius*1.25f, "quadratic_bezier", miya_brown);
    od_draw_text(renderer, cx-radius, cy+radius*1.25f, string, miya_brown);

    slot(18, &cx, &cy, &radius);
    float cubic_ctrl_pts[] = {cx, cy-radius*.8f, cx-radius, cy+radius*0.8f, cx, cy+radius, cx+radius*.8f, cy};
    num_capsules = od_draw_cubic_bezier(renderer, cubic_ctrl_pts, 20.f, miya_light_blue);
    snprintf(string, 256, "%u capsules", num_capsules);
    for(uint32_t i=0; i<4; i++)
        od_draw_disc(renderer, cubic_ctrl_pts[i*2], cubic_ctrl_pts[i*2+1], 10.f, miya_dark_green);
    od_draw_text(renderer, cx-radius, cy-radius*1.25f, "cubic_bezier", miya_brown);
    od_draw_text(renderer, cx-radius, cy+radius*1.25f, string, miya_brown);

    slot(19, &cx, &cy, &radius);
    od_begin_group(renderer, false, 0.f, 10.f);
    od_draw_box(renderer, cx-radius, cy-radius*.25f, cx+radius, cy+radius*.25f, 0.f, miya_blue);
    od_draw_disc(renderer, cx-radius*.5f, cy, radius*.3f, miya_dark_green);
    od_draw_sector(renderer, cx, cy, radius, angle, 0.78539816f, miya_pink);
    od_draw_arc(renderer, cx, cy, cosf(angle), sinf(angle), 0.78539816f, radius, radius * 0.1f, miya_red);
    od_end_group(renderer, miya_yellow);
    od_draw_text(renderer, cx-radius, cy-radius*1.25f, "outline", miya_brown);

    slot(20, &cx, &cy, &radius);
    od_set_clipdisc(renderer, cx, cy, radius);
    int seed = 0x12345678;
    const uint32_t colors[] = {miya_green, miya_pale_blue, miya_yellow};
    for(uint32_t i=0; i<100; i++)
    {
        float angle = iq_random_float(&seed) * 6.28f;
        od_draw_capsule(renderer, cx, cy, cx + cosf(angle) * 1000.f, cy + sinf(angle) * 1000.f, radius * 0.1f, colors[i%3]);
    }

    od_set_cliprect(renderer, 0.f, 0.f, UINT16_MAX, UINT16_MAX);
    od_draw_text(renderer, cx-radius, cy-radius*1.25f, "disc clip", miya_brown);

    slot(21, &cx, &cy, &radius);
    od_draw_capsule_gradient(renderer, cx - cosf(angle) * radius, cy - sinf(angle) * radius, cx + cosf(angle) * radius, cy + sinf(angle) * radius,
                             radius * 0.1f, miya_pale_blue, miya_red);
    od_draw_text(renderer, cx-radius, cy-radius*1.25f, "capsule_gradient", miya_brown);

    // a moving arrow, deterministic with the frame count : its position is a data float, so the hash
    // changes every frame and the tile cache must invalidate + redraw it (a stale tile would leave a
    // ghost and fail the PSNR check). 0.01 (1% of the width) keeps it a smooth drift, not a strobe
    {
        float s = fminf(sapp_widthf() / 8.f, sapp_heightf() / 3.375f) * 0.4f;
        float ax = fmodf(s + (float) frame_count * sapp_widthf() * 0.01f, sapp_widthf() + 2.f * s) - s;
        float ay = sapp_heightf() * 0.295f;

        od_begin_group(renderer, false, 0.f, 0.f);
        od_draw_box(renderer, ax - s, ay - s * 0.12f, ax + s * 0.3f, ay + s * 0.12f, 0.f, miya_red);
        // the pie's apex is at its center : put it at the tip (ax + 0.7s) and point the bisector back towards the shaft
        od_draw_sector(renderer, ax + s * 0.7f, ay, s * 0.4f, 3.14159265f - 0.6f, 1.2f, miya_red);
        od_end_group(renderer, miya_red);
    }

    slot(22, &cx, &cy, &radius);
    {
        od_dash_style round = {10.f, 10.f, 0.f, true};
        od_dash_style flat = {10.f, 10.f, 5.f, false};
        od_dash_style ants = {10.f, 10.f, (float) frame_count * 0.5f, true};    // marching ants, overlapping dashes, no double blend

        od_draw_dashed_line(renderer, cx - radius * 0.7f, cy - radius * 0.55f, cx + radius * 0.7f, cy - radius * 0.55f, 6.f, round, miya_red);
        od_draw_dashed_line(renderer, cx - radius * 0.7f, cy - radius * 0.25f, cx + radius * 0.7f, cy - radius * 0.25f, 6.f, flat, miya_blue);
        od_draw_dashed_ring(renderer, cx - radius * 0.45f, cy + radius * 0.15f, radius * 0.3f, 5.f, ants, 0x800000FF);
        od_draw_dashed_triangle_ring(renderer, (float[]){cx + radius * 0.15f, cy + radius * 0.05f, cx - radius * 0.05f, cy + radius * 0.25f,
                                                         cx + radius * 0.35f, cy + radius * 0.25f}, 5.f, round, miya_green);
        od_draw_dashed_ellipse_ring(renderer, cx + radius * 0.35f, cy + radius * 0.15f, cx + radius * 0.75f, cy + radius * 0.15f,
                                    radius * 0.25f, 5.f, flat, miya_pink);

        // the box family : sharp box, rounded box, oriented box (translucent, group wrapped)
        od_draw_dashed_box(renderer, cx - radius * 0.70f, cy + radius * 1.05f, cx - radius * 0.34f, cy + radius * 1.38f,
                           0.f, 5.f, flat, miya_dark_green);
        od_draw_dashed_box(renderer, cx - radius * 0.30f, cy + radius * 1.05f, cx + radius * 0.02f, cy + radius * 1.38f,
                           radius * 0.05f, 5.f, round, miya_pink);
        od_draw_dashed_oriented_box(renderer, cx + radius * 0.24f, cy + radius * 1.05f, cx + radius * 0.70f, cy + radius * 1.38f,
                                    radius * 0.16f, radius * 0.08f, 5.f, ants, 0x800000FF);

        // regression shapes in the bottom band :
        // a small ring with a dash longer than the radius (a dash must follow the curve, never cut it with a chord)
        od_draw_dashed_ring(renderer, cx - radius * 0.58f, cy + radius * 1.53f, radius * 0.10f, 5.f, (od_dash_style) {15.f, 10.f, 0.f, true}, miya_dark_blue);
        // a triangle and a box with an offset that lands mid edge (the walk must close back to the start)
        od_draw_dashed_triangle_ring(renderer, (float[]){cx - radius * 0.10f, cy + radius * 1.44f, cx - radius * 0.30f, cy + radius * 1.63f,
                                                         cx + radius * 0.10f, cy + radius * 1.63f}, 5.f, (od_dash_style) {10.f, 10.f, 7.f, true}, miya_red);
        od_draw_dashed_box(renderer, cx + radius * 0.18f, cy + radius * 1.44f, cx + radius * 0.50f, cy + radius * 1.62f, radius * 0.05f, 5.f,
                           (od_dash_style) {10.f, 10.f, 13.f, true}, miya_brown);
        // a solid rounded box (gap 0) : the corner arcs must be drawn, not skipped as sub half pixel chords
        od_draw_dashed_box(renderer, cx + radius * 0.60f, cy + radius * 1.44f, cx + radius * 0.92f, cy + radius * 1.62f, radius * 0.04f, 5.f,
                           (od_dash_style) {10.f, 0.f, 0.f, true}, miya_dark_grey);

        // a tiny dash on a huge path : overflows the command budget, must return UINT32_MAX fast, without emitting
        od_dash_style tiny = {0.001f, 0.001f, 0.f, true};
        uint32_t tiny_budget = od_draw_dashed_line(renderer, cx + radius * 0.7f, cy - radius * 0.55f, cx + radius * 0.7f + 3000.f, cy - radius * 0.55f, 6.f, tiny,
                                                   miya_grey);
        snprintf(string, 256, "dashed : %s", (tiny_budget == UINT32_MAX) ? "ok" : "error");
    }
    od_draw_text(renderer, cx-radius, cy-radius*1.25f, string, miya_brown);

    od_stats stats;
    od_get_stats(renderer, &stats);

    if (stats.frame_index > 60)
    {
        // average all gpu time to get close to the "real" value as the scene is static
        num_frames++;
        gpu_time_ms += stats.gpu_time_ms;

        snprintf(string, 256, "GPU Memory usage : %zu kb", stats.gpu_memory_usage>>10);
        od_draw_text(renderer, 0, sapp_heightf() - od_text_height(renderer) * 2.f, string, miya_blue);

        snprintf(string, 256, "num commands : %u", stats.peak_num_draw_cmd);
        od_draw_text(renderer, (sapp_widthf() - od_text_width(renderer, string)) * .5f,
                    sapp_heightf() - od_text_height(renderer) * 2.f, string, miya_blue);

        snprintf(string, 256, "GPU Time : %2.2f ms", gpu_time_ms / (float)(num_frames));
        od_draw_text(renderer, sapp_widthf() - od_text_width(renderer, string),
                    sapp_heightf() - od_text_height(renderer) * 2.f, string, miya_blue);
    }

    uint8_t* pixels = NULL;
    if (stats.frame_index == 10)
    {
        pixels = malloc(sapp_width() * sapp_height() * 4);
        od_take_screenshot(renderer, pixels);
    }

    sapp_swapchain sc = sapp_acquire_swapchain();
    od_end_frame(renderer, (void*)sc.metal.current_drawable);

    if (pixels != NULL)
    {
        bgra_to_rgba(pixels, sapp_width(), sapp_height());
        stbi_write_png("screenshot.png", sapp_width(), sapp_height(), 4, pixels,  sapp_width() * sizeof(uint32_t));
        compare_with_reference(pixels, sapp_width(), sapp_height());
        free(pixels);
    }

    if (options.autoexit && stats.frame_index == 100)
        sapp_quit();
}

// ---------------------------------------------------------------------------------------------------------------------------
void event(const sapp_event* event)
{
    if (renderer == NULL)
        return;

    switch(event->type)
    {
    case SAPP_EVENTTYPE_RESIZED : 
        od_resize(renderer, event->framebuffer_width, event->framebuffer_height);
        break;
    case SAPP_EVENTTYPE_KEY_DOWN : 
    {
        if (event->key_code == SAPP_KEYCODE_Q && event->modifiers == SAPP_MODIFIER_SUPER)
            sapp_quit();

        if (event->key_code == SAPP_KEYCODE_D && event->modifiers == SAPP_MODIFIER_SUPER)
        {
            culling_debug = !culling_debug;
            od_set_culling_debug(renderer, culling_debug);
        } 

        break;
    }

            
    default: break;
    }
}

// ---------------------------------------------------------------------------------------------------------------------------
void cleanup(void)
{
    if (renderer != NULL)
        od_terminate(renderer);
    free(renderer);
    renderer = NULL;
}

// ---------------------------------------------------------------------------------------------------------------------------
sapp_desc sokol_main(int argc, char* argv[])
{
    // flag.h matches single-dash names; normalize "--foo" to "-foo" so both forms work
    for (int i = 1; i < argc; ++i)
    {
        if (argv[i][0] == '-' && argv[i][1] == '-' && argv[i][2] != '\0')
            memmove(argv[i] + 1, argv[i] + 2, strlen(argv[i]) - 1);
    }

    flag_bool_var(&options.autoexit, "autoexit", false, "quit after 100 frames");
    flag_bool_var(&options.compare, "compare", false, "exit non-zero if the screenshot differs from the reference");
    flag_bool_var(&options.update_refs, "update-refs", false, "rewrite the matching reference image from the current capture");
    flag_bool_var(&options.cache, "cache", false, "enable the tile cache");

    if (!flag_parse(argc, argv))
    {
        flag_print_error(stderr);
        flag_print_options(stderr);
        exit(1);
    }
    return (sapp_desc) 
    {
        .width = 1280,
        .height = 720,
        .init_cb = init,
        .frame_cb = frame,
        .cleanup_cb = cleanup,
        .event_cb = event,
        .high_dpi = true,
        .window_title = "onedraw"
    };
}
