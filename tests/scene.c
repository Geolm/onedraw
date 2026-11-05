#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#include "scene.h"

#define LABEL_SIZE (CANVAS_HEIGHT / 30.f)   // 48 canvas px : 24 device px at 1x, 48 at 2x, the exact look of the old baked atlas

// https://lospec.com/palette-list/miyazaki-16
const uint32_t miya_black = FROM_HTML(0x232228);
const uint32_t miya_dark_blue = FROM_HTML(0x284261);
const uint32_t miya_dark_grey = FROM_HTML(0x5f5854);
const uint32_t miya_grey = FROM_HTML(0x878573);
const uint32_t miya_light_grey = FROM_HTML(0xb8b095);
const uint32_t miya_pale_blue = FROM_HTML(0xc3d5c7);
const uint32_t miya_white = FROM_HTML(0xebecdc);
const uint32_t miya_blue = FROM_HTML(0x2485a6);
const uint32_t miya_light_blue = FROM_HTML(0x54bad2);
const uint32_t miya_brown = FROM_HTML(0x754d45);
const uint32_t miya_red = FROM_HTML(0xc65046);
const uint32_t miya_pink = FROM_HTML(0xe6928a);
const uint32_t miya_dark_green = FROM_HTML(0x1e7453);
const uint32_t miya_green = FROM_HTML(0x55a058);
const uint32_t miya_light_green = FROM_HTML(0xa1bf41);
const uint32_t miya_yellow = FROM_HTML(0xe3c054);

static const float angle = 0.78539816f;

//----------------------------------------------------------------------------------------------------------------------------
static inline draw_color patch_color_alpha(draw_color input, uint8_t alpha)
{
    return (input&0x00ffffff) | (alpha<<24);
}

//----------------------------------------------------------------------------------------------------------------------------
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

//----------------------------------------------------------------------------------------------------------------------------
static void make_checker(uint32_t* p, uint32_t w, uint32_t h, uint32_t a, uint32_t b)
{
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            *p++ = (((x >> 5) ^ (y >> 5)) & 1) ? a : b;
}

//----------------------------------------------------------------------------------------------------------------------------
static void make_rings(uint32_t* p, uint32_t w, uint32_t h, uint32_t a, uint32_t b)
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

//----------------------------------------------------------------------------------------------------------------------------
static void make_hgradient(uint32_t* p, uint32_t w, uint32_t h, uint32_t a, uint32_t b)
{
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
        {
            float t = (float)x / (float)(w - 1);
            *p++ = lerp_color(a, b, t);
        }
}

//----------------------------------------------------------------------------------------------------------------------------
static void make_radial(uint32_t* p, uint32_t w, uint32_t h, uint32_t a, uint32_t b)
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

//----------------------------------------------------------------------------------------------------------------------------
static void fill_texture_array(struct onedraw* r)
{
    if (r == NULL)
        return;

    uint32_t* pixel_data = malloc(TEX_SIZE * TEX_SIZE * sizeof(uint32_t));

    make_checker(pixel_data, TEX_SIZE, TEX_SIZE, miya_green, miya_yellow);
    od_upload_slice(r, pixel_data, 0);

    make_hgradient(pixel_data, TEX_SIZE, TEX_SIZE, miya_dark_blue, miya_light_blue);
    od_upload_slice(r, pixel_data, 1);

    make_rings(pixel_data, TEX_SIZE, TEX_SIZE, miya_brown, miya_pink);
    od_upload_slice(r, pixel_data, 2);

    make_radial(pixel_data, TEX_SIZE, TEX_SIZE, miya_black, miya_light_green);
    od_upload_slice(r, pixel_data, 3);

    free(pixel_data);
}

//----------------------------------------------------------------------------------------------------------------------------
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
// SplitMix32 PRNG implementation
static inline uint32_t splitmix32(uint32_t* state)
{
    uint32_t z = (*state += 0x9E3779B9u);
    z = (z ^ (z >> 16)) * 0x85EBCA6Bu;
    z = (z ^ (z >> 13)) * 0xC2B2AE35u;
    return z ^ (z >> 16);
}

//----------------------------------------------------------------------------------------------------------------------------
// Returns a random float in [0; 1[
static inline float splitmix32_float(uint32_t* state)
{
    union
    {
        uint32_t i;
        float f;
    } u;

    *state += 0x9e3779b9u;
    uint64_t z = *state;
    z = (z ^ (z >> 15)) * 0x85ebca6bULL;
    z = (z ^ (z >> 13)) * 0xc2b2ae35ULL;
    z ^= z >> 16;

    uint32_t mant = (uint32_t)(z >> 9) & 0x007FFFFFu;
    u.i = mant | 0x3f800000u;
    return u.f - 1.0f;
}

//----------------------------------------------------------------------------------------------------------------------------
// A faint, non-overlapping field of triangles/discs behind the feature shapes, to give the scene
// density (command, tile and node counts) for the memory & binning plan steps. One shape per grid
// cell, kept inside its cell so neighbours never touch ; a fixed seed keeps the reference stable.
static void draw_background(struct onedraw* r)
{
    if (r == NULL)
        return;

    const float w = CANVAS_WIDTH;
    const float h = CANVAS_HEIGHT;
    const float cell = 56.f;
    const float max_jitter = cell * 0.16f;
    const float min_r = cell * 0.22f;
    const float max_r = cell * 0.30f;

    const uint32_t alpha = 0x30;
    const uint32_t colors[] =
    {
        (miya_dark_blue & 0x00ffffffu) | (alpha << 24),
        (miya_green & 0x00ffffffu) | (alpha << 24),
        (miya_yellow & 0x00ffffffu) | (alpha << 24),
        (miya_red & 0x00ffffffu) | (alpha << 24),
        (miya_dark_grey & 0x00ffffffu) | (alpha << 24),
    };
    const uint32_t num_colors = (uint32_t)(sizeof(colors) / sizeof(colors[0]));

    uint32_t state = 0x5eed5eedu;

    for (float gy = 0.f; gy < h; gy += cell)
    {
        for (float gx = 0.f; gx < w; gx += cell)
        {
            float cx = gx + cell * .5f + (splitmix32_float(&state) * 2.f - 1.f) * max_jitter;
            float cy = gy + cell * .5f + (splitmix32_float(&state) * 2.f - 1.f) * max_jitter;
            float radius = min_r + splitmix32_float(&state) * (max_r - min_r);
            uint32_t color = colors[splitmix32(&state) % num_colors];

            if (splitmix32(&state) & 1u)
            {
                float base = splitmix32_float(&state) * 6.2831853f;
                float verts[6];
                for (int i = 0; i < 3; i++)
                {
                    float a = base + i * 2.0943951f;
                    verts[i*2]   = cx + cosf(a) * radius;
                    verts[i*2+1] = cy + sinf(a) * radius;
                }
                od_draw_triangle(r, verts, radius * 0.15f, color);
            }
            else
                od_draw_disc(r, cx, cy, radius, color);
        }
    }
}

//----------------------------------------------------------------------------------------------------------------------------
static void slot(uint32_t index, float* cx, float* cy, float* radius)
{
    float step_x = CANVAS_WIDTH / 8.f;
    float step_y = CANVAS_HEIGHT / 3.375f;

    *cx = (index%8) * step_x + step_x * .5f;
    *cy = (index/8) * step_y + step_y * .5f;
    *radius = fminf(step_x, step_y) * .4f;
}

// ---------------------------------------------------------------------------------------------------------------------------
void scene_init(struct onedraw* r)
{
    if (r == NULL)
        return;

    od_set_clear_color(r, miya_white);

    od_set_canvas(r, 0.f, 0.f, CANVAS_WIDTH, CANVAS_HEIGHT);
    fill_texture_array(r);
}

// ---------------------------------------------------------------------------------------------------------------------------
void scene_draw(struct onedraw* r, uint32_t frame_count)
{
    if (r == NULL)
        return;

    float cx, cy, radius;
    char string[256];

    draw_background(r);

    slot(0, &cx, &cy, &radius);
    od_draw_disc_gradient(r, cx, cy, radius, miya_dark_blue, miya_light_blue);
    od_draw_text(r, cx-radius, cy-radius*1.25f, "disc_gradient", od_font_bold, LABEL_SIZE, miya_brown);

    slot(1, &cx, &cy, &radius);
    od_draw_ring(r, cx, cy, radius, radius * .1f, miya_green);
    od_draw_text(r, cx-radius, cy-radius*1.25f, "ring", od_font_bold, LABEL_SIZE, miya_brown);

    slot(2, &cx, &cy, &radius);
    od_draw_box(r, cx - radius, cy - radius*.5f, cx + radius, cy + radius*.5f, radius * 0.05f, patch_color_alpha(miya_grey, 224));
    od_draw_text(r, cx-radius, cy-radius*1.25f, "box", od_font_bold, LABEL_SIZE, miya_brown);

    slot(3, &cx, &cy, &radius);
    od_draw_blurred_box(r, cx, cy, radius*.5f, radius, radius * 0.1f, miya_black);
    od_draw_text(r, cx-radius, cy-radius*1.25f, "blurred_box", od_font_bold, LABEL_SIZE, miya_brown);

    slot(4, &cx, &cy, &radius);
    od_draw_oriented_rect(r, cx - cosf(angle) * radius, cy - sinf(angle) * radius, cx + cosf(angle) * radius, cy + sinf(angle) * radius,
                          radius * 0.4f, 0.f, radius * 0.1f, miya_pale_blue);
    od_draw_text(r, cx-radius, cy-radius*1.25f, "oriented_rect", od_font_bold, LABEL_SIZE, miya_brown);

    slot(5, &cx, &cy, &radius);
    od_draw_oriented_box(r, cx + cosf(angle) * radius, cy - sinf(angle) * radius, cx - cosf(angle) * radius, cy + sinf(angle) * radius,
                         radius * 0.5f, radius * 0.05f, patch_color_alpha(miya_red, 224));
    od_draw_text(r, cx-radius, cy-radius*1.25f, "oriented_box", od_font_bold, LABEL_SIZE, miya_brown);

    slot(6, &cx, &cy, &radius);
    od_draw_triangle(r, (float[]){cx, cy, cx - cosf(angle) * radius, cy + sinf(angle) * radius,
                     cx + cosf(angle) * radius, cy +sinf(angle) * radius}, radius * 0.1f, patch_color_alpha(miya_dark_green, 224));
    od_draw_text(r, cx-radius, cy-radius*1.25f, "triangle", od_font_bold, LABEL_SIZE, miya_brown);

    slot(7, &cx, &cy, &radius);
    od_draw_triangle_ring(r, (float[]){cx, cy, cx - cosf(angle) * radius, cy - sinf(angle) * radius,
                          cx + cosf(angle) * radius, cy - sinf(angle) * radius}, 0.f, radius * 0.1f, miya_dark_grey);
    od_draw_text(r, cx-radius, cy-radius*1.25f, "triangle_ring", od_font_bold, LABEL_SIZE, miya_brown);

    slot(8, &cx, &cy, &radius);
    od_draw_ellipse(r, cx + cosf(angle) * radius, cy - sinf(angle) * radius, cx - cosf(angle) * radius, cy + sinf(angle) * radius,
                    radius, patch_color_alpha(miya_yellow, 224));
    od_draw_text(r, cx-radius, cy-radius*1.25f, "ellipse", od_font_bold, LABEL_SIZE, miya_brown);

    slot(9, &cx, &cy, &radius);
    od_draw_ellipse_ring(r, cx + cosf(angle) * radius, cy - sinf(angle) * radius, cx - cosf(angle) * radius, cy + sinf(angle) * radius,
                         radius, radius * 0.1f, miya_light_grey);
    od_draw_text(r, cx-radius, cy-radius*1.25f, "ellipse_ring", od_font_bold, LABEL_SIZE, miya_brown);

    slot(10, &cx, &cy, &radius);
    od_draw_sector(r, cx, cy, radius, angle, 0.78539816f, miya_pink);
    od_draw_text(r, cx-radius, cy-radius*1.25f, "sector", od_font_bold, LABEL_SIZE, miya_brown);

    slot(11, &cx, &cy, &radius);
    od_draw_sector_ring(r, cx, cy, radius, -angle, 0.78539816f, radius * 0.1f, miya_dark_blue);
    od_draw_text(r, cx-radius, cy-radius*1.25f, "sector_ring", od_font_bold, LABEL_SIZE, miya_brown);

    slot(12, &cx, &cy, &radius);
    od_draw_arc(r, cx, cy, cosf(angle), sinf(angle), 0.78539816f, radius, radius * 0.1f, miya_red);
    od_draw_text(r, cx-radius, cy-radius*1.25f, "arc", od_font_bold, LABEL_SIZE, miya_brown);

    slot(13, &cx, &cy, &radius);
    {
        // the multi-face text slot : every family at its own size, then a line that toggles face and
        // size on a frame cadence (deterministic like the moving arrow) : its draw data changes,
        // so the tile cache must redraw from the changed command hash (font.generation is gone,
        // the draw-data hash is the only invalidation source now)
        const od_font faces[4] = {od_font_regular, od_font_bold, od_font_italic, od_font_mono};
        const float sizes[4] = {24.f, 32.f, 24.f, 16.f};
        const char* strings[4] = {"The quick brown fox", "Bold heading", "Italic emphasis", "const int x = 42;"};

        od_canvas_cliprect(r, cx - radius, cy - radius, cx + radius, cy + radius);

        float py = cy - radius;
        for (int i = 0; i < 4; ++i)
        {
            od_draw_text(r, cx - radius, py, strings[i], faces[i], sizes[i], miya_black);
            py += od_text_line_height(r, faces[i], sizes[i]);
        }

        uint32_t toggle = (frame_count / 30) % 4;
        const float toggle_sizes[4] = {24.f, 32.f, 48.f, 16.f};
        // the "toggle" word is repeated so the 6-byte view never ends on a null byte : it must be
        // length delimited, not null terminated
        static const char toggle_text[] = "toggletoggletoggletoggletoggle";
        od_draw_string_view(r, cx - radius, py, toggle_text + toggle * 6, 6, faces[toggle], toggle_sizes[toggle], miya_black);

        od_pop_clip(r);
    }
    od_draw_text(r, cx-radius, cy-radius*1.25f, "text", od_font_bold, LABEL_SIZE, miya_brown);

    slot(14, &cx, &cy, &radius);
    od_begin_group(r, true, radius * 0.25f, radius * 0.05f);
    od_draw_disc(r, cx, cy, radius*0.25f, patch_color_alpha(miya_light_green, 224));
    od_draw_disc(r, cx + cosf(angle) * radius * .5f, cy - sinf(angle) * radius * .5f, radius*0.25f, patch_color_alpha(miya_light_green, 224));
    od_draw_box(r, cx-radius*.7f, cy-radius*.7f, cx-radius*.3f, cy+radius*.7f, 0.f, patch_color_alpha(miya_yellow, 224));
    od_end_group(r, miya_brown);
    od_draw_text(r, cx-radius, cy-radius*1.25f, "smoothmin", od_font_bold, LABEL_SIZE, miya_brown);

    slot(15, &cx, &cy, &radius);
    od_canvas_cliprect_rounded(r, cx - radius*.5f, cy - radius*.5f, cx + radius*.5f, cy + radius*.5f, radius*.25f);
    od_draw_quad(r, cx-radius, cy-radius, cx, cy, (od_quad_uv){0.f, 0.f, 1.f, 1.f}, 0, 0xafffffff);
    od_draw_quad(r, cx, cy-radius, cx+radius, cy, (od_quad_uv){0.f, 0.f, 1.f, 1.f}, 1, 0xafffffff);
    od_draw_quad(r, cx-radius, cy, cx, cy+radius, (od_quad_uv){0.f, 0.f, 1.f, 1.f}, 2, 0xafffffff);
    od_draw_quad(r, cx, cy, cx+radius, cy+radius, (od_quad_uv){0.f, 0.f, 1.f, 1.f}, 3, 0xafffffff);
    od_pop_clip(r);
    {
        const float label_height = od_text_line_height(r, od_font_bold, LABEL_SIZE);
        od_draw_text(r, cx-radius, cy-radius*1.25f, "quad", od_font_bold, LABEL_SIZE, miya_brown);
        od_draw_text(r, cx-radius, cy-radius*1.25f + label_height, "rounded clip", od_font_bold, LABEL_SIZE, miya_brown);
    }

    slot(16, &cx, &cy, &radius);
    od_draw_oriented_quad(r, cx, cy, radius, radius*.5f, angle * 0.75f, (od_quad_uv){0.f, 0.f, 1.f, 0.5f}, 2, 0xafffffff);
    od_draw_text(r, cx-radius, cy-radius*1.25f, "oriented_quad", od_font_bold, LABEL_SIZE, miya_brown);

    slot(17, &cx, &cy, &radius);
    float quadratic_ctrl_pts[] = {cx, cy-radius*.8f, cx-radius, cy+radius*0.8f, cx, cy+radius};
    uint32_t num_capsules = od_draw_quadratic_bezier(r, quadratic_ctrl_pts, 20.f, miya_red);
    snprintf(string, 256, "%u capsules", num_capsules);
    for(uint32_t i=0; i<3; i++)
        od_draw_disc(r, quadratic_ctrl_pts[i*2], quadratic_ctrl_pts[i*2+1], 10.f, miya_yellow);
    od_draw_text(r, cx-radius, cy-radius*1.25f, "quadratic_bezier", od_font_bold, LABEL_SIZE, miya_brown);
    od_draw_text(r, cx-radius, cy+radius*1.25f, string, od_font_bold, LABEL_SIZE, miya_brown);

    slot(18, &cx, &cy, &radius);
    float cubic_ctrl_pts[] = {cx, cy-radius*.8f, cx-radius, cy+radius*0.8f, cx, cy+radius, cx+radius*.8f, cy};
    num_capsules = od_draw_cubic_bezier(r, cubic_ctrl_pts, 20.f, miya_light_blue);
    snprintf(string, 256, "%u capsules", num_capsules);
    for(uint32_t i=0; i<4; i++)
        od_draw_disc(r, cubic_ctrl_pts[i*2], cubic_ctrl_pts[i*2+1], 10.f, miya_dark_green);
    od_draw_text(r, cx-radius, cy-radius*1.25f, "cubic_bezier", od_font_bold, LABEL_SIZE, miya_brown);
    od_draw_text(r, cx-radius, cy+radius*1.25f, string, od_font_bold, LABEL_SIZE, miya_brown);

    slot(19, &cx, &cy, &radius);
    od_begin_group(r, false, 0.f, 10.f);
    od_draw_box(r, cx-radius, cy-radius*.25f, cx+radius, cy+radius*.25f, 0.f, miya_blue);
    od_draw_disc(r, cx-radius*.5f, cy, radius*.3f, miya_dark_green);
    od_draw_sector(r, cx, cy, radius, angle, 0.78539816f, miya_pink);
    od_draw_arc(r, cx, cy, cosf(angle), sinf(angle), 0.78539816f, radius, radius * 0.1f, miya_red);
    od_end_group(r, miya_yellow);
    od_draw_text(r, cx-radius, cy-radius*1.25f, "outline", od_font_bold, LABEL_SIZE, miya_brown);

    slot(20, &cx, &cy, &radius);
    od_canvas_clipdisc(r, cx, cy, radius);
    int seed = 0x12345678;
    const uint32_t colors[] = {miya_green, miya_pale_blue, miya_yellow};
    for(uint32_t i=0; i<100; i++)
    {
        float angle = iq_random_float(&seed) * 6.28f;
        od_draw_capsule(r, cx, cy, cx + cosf(angle) * 1000.f, cy + sinf(angle) * 1000.f, radius * 0.1f, colors[i%3]);
    }

    od_canvas_cliprect(r, cx - radius, cy - radius, cx + radius, cy + radius);
    od_draw_box(r, cx - radius, cy - radius, cx - radius * 0.55f, cy - radius * 0.55f, 0.f, miya_red);
    od_pop_clip(r);
    od_draw_box(r, cx + radius * 0.75f, cy - radius * 0.95f, cx + radius * 0.95f, cy - radius * 0.75f, 0.f, miya_red);
    od_pop_clip(r);
    od_draw_text(r, cx-radius, cy-radius*1.25f, "disc clip", od_font_bold, LABEL_SIZE, miya_brown);

    slot(21, &cx, &cy, &radius);
    od_draw_capsule_gradient(r, cx - cosf(angle) * radius, cy - sinf(angle) * radius, cx + cosf(angle) * radius, cy + sinf(angle) * radius,
                             radius * 0.1f, miya_pale_blue, miya_red);
    od_draw_text(r, cx-radius, cy-radius*1.25f, "capsule_gradient", od_font_bold, LABEL_SIZE, miya_brown);

    // a moving arrow, deterministic with the frame count : its position is a data float, so the hash
    // changes every frame and the tile cache must invalidate + redraw it (a stale tile would leave a
    // ghost and fail the PSNR check). 0.01 (1% of the width) keeps it a smooth drift, not a strobe
    {
        float s = fminf(CANVAS_WIDTH / 8.f, CANVAS_HEIGHT / 3.375f) * 0.4f;
        float ax = fmodf(s + (float) frame_count * CANVAS_WIDTH * 0.01f, CANVAS_WIDTH + 2.f * s) - s;
        float ay = CANVAS_HEIGHT * 0.295f;

        od_begin_group(r, false, 0.f, 2.f);
        draw_color arrow_color = patch_color_alpha(miya_red, 224);
        od_draw_box(r, ax - s, ay - s * 0.12f, ax + s * 0.3f, ay + s * 0.12f, 0.f, arrow_color);
        // the pie's apex is at its center : put it at the tip (ax + 0.7s) and point the bisector back towards the shaft
        od_draw_sector(r, ax + s * 0.6f, ay, s * 0.4f, 3.14159265f - 0.6f, 1.2f, arrow_color);
        od_end_group(r, patch_color_alpha(miya_dark_grey, 224));
    }

    slot(22, &cx, &cy, &radius);
    {
        od_dash_style round = {10.f, 10.f, 0.f, true};
        od_dash_style flat = {10.f, 10.f, 5.f, false};
        od_dash_style ants = {10.f, 10.f, (float) frame_count * 0.5f, true};    // marching ants, overlapping dashes, no double blend

        od_draw_dashed_line(r, cx - radius * 0.7f, cy - radius * 0.55f, cx + radius * 0.7f, cy - radius * 0.55f, 6.f, round, miya_red);
        od_draw_dashed_line(r, cx - radius * 0.7f, cy - radius * 0.25f, cx + radius * 0.7f, cy - radius * 0.25f, 6.f, flat, miya_blue);
        od_draw_dashed_ring(r, cx - radius * 0.45f, cy + radius * 0.15f, radius * 0.3f, 5.f, ants, 0x800000FF);
        od_draw_dashed_triangle_ring(r, (float[]){cx + radius * 0.15f, cy + radius * 0.05f, cx - radius * 0.05f, cy + radius * 0.25f,
                                                         cx + radius * 0.35f, cy + radius * 0.25f}, 5.f, round, miya_green);
        od_draw_dashed_ellipse_ring(r, cx + radius * 0.35f, cy + radius * 0.15f, cx + radius * 0.75f, cy + radius * 0.15f,
                                    radius * 0.25f, 5.f, flat, miya_pink);

        // the box family : sharp box, rounded box, oriented box (translucent, group wrapped)
        od_draw_dashed_box(r, cx - radius * 0.70f, cy + radius * 1.05f, cx - radius * 0.34f, cy + radius * 1.38f,
                           0.f, 5.f, flat, miya_dark_green);
        od_draw_dashed_box(r, cx - radius * 0.30f, cy + radius * 1.05f, cx + radius * 0.02f, cy + radius * 1.38f,
                           radius * 0.05f, 5.f, round, miya_pink);
        od_draw_dashed_oriented_box(r, cx + radius * 0.24f, cy + radius * 1.05f, cx + radius * 0.70f, cy + radius * 1.38f,
                                    radius * 0.16f, radius * 0.08f, 5.f, ants, 0x800000FF);

        
        od_draw_dashed_ring(r, cx - radius * 0.58f, cy + radius * 1.53f, radius * 0.10f, 5.f, (od_dash_style) {15.f, 10.f, 0.f, true}, miya_dark_blue);
        
        od_draw_dashed_triangle_ring(r, (float[]){cx - radius * 0.10f, cy + radius * 1.44f, cx - radius * 0.30f, cy + radius * 1.63f,
                                                         cx + radius * 0.10f, cy + radius * 1.63f}, 5.f, (od_dash_style) {10.f, 10.f, 7.f, true}, miya_red);
        od_draw_dashed_box(r, cx + radius * 0.18f, cy + radius * 1.44f, cx + radius * 0.50f, cy + radius * 1.62f, radius * 0.05f, 5.f,
                           (od_dash_style) {10.f, 10.f, 13.f, true}, miya_brown);
        
        od_draw_dashed_box(r, cx + radius * 0.60f, cy + radius * 1.44f, cx + radius * 0.92f, cy + radius * 1.62f, radius * 0.04f, 5.f,
                           (od_dash_style) {10.f, 0.f, 0.f, true}, miya_dark_grey);

        // a tiny dash on a huge path : overflows the command budget, must return UINT32_MAX fast, without emitting
        od_dash_style tiny = {0.001f, 0.001f, 0.f, true};
        uint32_t tiny_budget = od_draw_dashed_line(r, cx + radius * 0.7f, cy - radius * 0.55f, cx + radius * 0.7f + 3000.f, cy - radius * 0.55f, 6.f, tiny,
                                                   miya_grey);
        snprintf(string, 256, "dashed : %s", (tiny_budget == UINT32_MAX) ? "ok" : "error");
    }
    od_draw_text(r, cx-radius, cy-radius*1.25f, string, od_font_bold, LABEL_SIZE, miya_brown);

    slot(23, &cx, &cy, &radius);
    {
        const uint32_t grid_colors[] = {miya_green, miya_yellow, miya_pink, miya_red, miya_blue, miya_light_green};
        const uint32_t num_cols = 6;
        const uint32_t num_rows = 5;

        for (uint32_t j = 0; j < num_rows; j++)
        {
            for (uint32_t i = 0; i < num_cols; i++)
            {
                float gx = cx + ((float)i - (float)(num_cols - 1) * .5f) * radius * .36f;
                float gy = cy + ((float)j - (float)(num_rows - 1) * .5f) * radius * .3f;
                uint32_t color = grid_colors[(i + j) % 6u];

                if ((i + j) & 1u)
                    od_draw_disc(r, gx, gy, radius * .1f, color);
                else
                    od_draw_box(r, gx - radius * .1f, gy - radius * .1f, gx + radius * .1f, gy + radius * .1f, 0.f, color);
            }
        }

        od_draw_box(r, cx - radius * 1.1f, cy - radius * 0.8f, cx + radius * 1.1f, cy + radius * 1.4f, radius * 0.2f, miya_blue);
        od_draw_box(r, cx - radius * 1.0f, cy - radius * 0.7f, cx + radius * 1.0f, cy + radius * 1.3f, 0.f, miya_white);
        od_draw_disc(r, cx + cosf(angle) * radius * .3f, cy - sinf(angle) * radius * .3f, radius * .33f, miya_red);
    }
    od_draw_text(r, cx-radius, cy-radius*1.25f, "occlusion", od_font_bold, LABEL_SIZE, miya_brown);
}
