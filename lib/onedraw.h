/*

    onedraw — a GPU-driven 2D renderer

    Project URL : https://github.com/Geolm/onedraw


    zlib License

    (C) 2025 Geolm

    This software is provided 'as-is', without any express or implied
    warranty.  In no event will the authors be held liable for any damages
    arising from the use of this software.

    Permission is granted to anyone to use this software for any purpose,
    including commercial applications, and to alter it and redistribute it
    freely, subject to the following restrictions:

    1. The origin of this software must not be misrepresented; you must not
    claim that you wrote the original software. If you use this software
    in a product, an acknowledgment in the product documentation would be
    appreciated but is not required.
    2. Altered source versions must be plainly marked as such, and must not be
    misrepresented as being the original software.
    3. This notice may not be removed or altered from any source distribution.

*/


#ifndef __ONEDRAW_H__
#define __ONEDRAW_H__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

//----------------------------------------------------------------------------------------------------------------------------
// Structures
//----------------------------------------------------------------------------------------------------------------------------

struct onedraw;

typedef struct od_quad_uv
{
    float tl_u, tl_v;   // top-left uv
    float br_u, br_v;   // bottom-right uv;
} od_quad_uv;

typedef struct od_stats
{
    uint32_t frame_index;
    uint32_t num_draw_cmd;
    uint32_t peak_num_draw_cmd;
    float gpu_time_ms;
} od_stats;

typedef struct od_glyph
{
    uint16_t x0, y0, x1, y1;
    float bearing_x, bearing_y;
    float advance_x;
} od_glyph;

typedef struct onedraw_def
{
    void* preallocated_buffer;
    void* metal_device;
    uint32_t viewport_width;
    uint32_t viewport_height;
    void (*log_func)(const char* string);
    bool allow_screenshot;
    bool srgb_backbuffer;

    struct
    {
        uint32_t width, height;
        uint32_t num_slices;        // Max 256
    } texture_array;

} onedraw_def;

typedef uint32_t draw_color; // color is expected to be B8G8R8A8 and in sRGB color space

#ifdef __cplusplus
extern "C" {
#endif

//----------------------------------------------------------------------------------------------------------------------------
// API
//----------------------------------------------------------------------------------------------------------------------------

//-----------------------------------------------------------------------------------------------------------------------------
// Returns the number of bytes needed by the library 
// ==> as the lib allocates gpu buffers, the total memory usage is higher than this and depends on resolution
size_t od_min_memory_size();

//-----------------------------------------------------------------------------------------------------------------------------
// Initializes the library
//      [preallocated_buffer]   user-allocated memory of od_min_memory_size() bytes, must be aligned on sizeof(uintptr_t)
//      [metal_device]          pointer to (MTL::Device*) device or obj-C equivalent
//      [viewport_width]
//      [viewport_height]
//      [log_func]              pointer to the log function, can be NULL if no log required
//      [allow_screenshot]      if true buffers are allocated for screenshot
//      [texture_array]
//          [width]             width of all textures in the array, if 0 (undefined) the array won't be created
//          [height]            
//          [num_slices]        must be < 256. each quad can use a specific slice. 
struct onedraw* od_init(onedraw_def* def);

//-----------------------------------------------------------------------------------------------------------------------------
// Uploads (or replaces) a slice of the texture array
//      [pixel_data]            pointer to the pixel data in B8G8R8A8_srgb format
//      [slice_index]           must be < num_slices
//
// warning: textures are stored in shared memory.
//          updating a slice while it’s being sampled by the GPU may cause flickering or corruption.
//          the user is responsible for synchronizing uploads.
void od_upload_slice(struct onedraw* r, const void* pixel_data, uint32_t slice_index);

//-----------------------------------------------------------------------------------------------------------------------------
// Sets-up the capture region for screenshots
void od_capture_region(struct onedraw* r, uint32_t x, uint32_t y, uint32_t width, uint32_t height);

//-----------------------------------------------------------------------------------------------------------------------------
// Writes a tga screenshot in the current folder, must be called before between begin/end frame
void od_take_screenshot(struct onedraw* r);

//-----------------------------------------------------------------------------------------------------------------------------
// Resizes the renderer output dimensions (call this when the window size changes)
void od_resize(struct onedraw* r, uint32_t width, uint32_t height);

//-----------------------------------------------------------------------------------------------------------------------------
// Begins a new frame. Must be called before any drawing functions is called.
void od_begin_frame(struct onedraw* r);

//-----------------------------------------------------------------------------------------------------------------------------
// Ends the collect of draw commands, renders everything
//      [drawable]              pointer to CA::MetalDrawable* from obj-C
void od_end_frame(struct onedraw* r, void* drawable);

//-----------------------------------------------------------------------------------------------------------------------------
// Returns the average gpu time for the last 60 frames
float od_get_average_gputime(struct onedraw* r);

//-----------------------------------------------------------------------------------------------------------------------------
// Frees GPU and CPU memory used by the renderer
void od_terminate(struct onedraw* r);

//-----------------------------------------------------------------------------------------------------------------------------
// Fill the od_stats structure with latest data
//      [stats]     non-NULL pointer to the structure
void od_get_stats(struct onedraw* r, od_stats* stats);

//-----------------------------------------------------------------------------------------------------------------------------
// Sets the clear color
void od_set_clear_color(struct onedraw* r, draw_color srgb_color);

//-----------------------------------------------------------------------------------------------------------------------------
// Sets the clip rectangle
//  ==> There is a limit of MAX_CLIPS
void od_set_cliprect(struct onedraw* r, uint16_t min_x, uint16_t min_y, uint16_t max_x, uint16_t max_y);

//-----------------------------------------------------------------------------------------------------------------------------
// Outputs a blue color as the background of each tile. Mainly use to debug binning.
void od_set_culling_debug(struct onedraw* r, bool b);

void od_begin_group(struct onedraw* r, bool smoothblend, float smooth_value, float outline_width);
void od_end_group(struct onedraw* r, draw_color outline_color);

float od_text_height(struct onedraw* r);
float od_text_width(struct onedraw* r, const char* text);

void od_draw_ring(struct onedraw* r, float cx, float cy, float radius, float thickness, draw_color srgb_color);
void od_draw_disc(struct onedraw* r, float cx, float cy, float radius, draw_color srgb_color);
void od_draw_oriented_box(struct onedraw* r, float ax, float ay, float bx, float by, float width, float roundness, draw_color srgb_color);
void od_draw_oriented_rect(struct onedraw* r, float ax, float ay, float bx, float by, float width, float roundness, float thickness, draw_color srgb_color);
void od_draw_line(struct onedraw* r, float ax, float ay, float bx, float by, float width, draw_color srgb_color);
void od_draw_ellipse(struct onedraw* r, float ax, float ay, float bx, float by, float width, draw_color srgb_color);
void od_draw_ellipse_ring(struct onedraw* r, float ax, float ay, float bx, float by, float width, float thickness, draw_color srgb_color);
void od_draw_triangle(struct onedraw* r, const float* vertices, float roundness, draw_color srgb_color);
void od_draw_triangle_ring(struct onedraw* r, const float* vertices, float roundness, float thickness, draw_color srgb_color);
void od_draw_sector(struct onedraw* r, float cx, float cy, float radius, float start_angle, float sweep_angle, draw_color srgb_color);
void od_draw_sector_ring(struct onedraw* r, float cx, float cy, float radius, float start_angle, float sweep_angle, float thickness, draw_color srgb_color);
void od_draw_arc(struct onedraw* r, float cx, float cy, float dx, float dy, float aperture, float radius, float thickness, draw_color srgb_color);
void od_draw_box(struct onedraw* r, float x0, float y0, float x1, float y1, float radius, draw_color srgb_color);
void od_draw_blurred_box(struct onedraw* r, float cx, float cy, float half_width, float half_height, float roundness, draw_color srgb_color);
void od_draw_char(struct onedraw* r, float x, float y, char c, draw_color srgb_color);
void od_draw_text(struct onedraw* r, float x, float y, const char* text, draw_color srgb_color);
void od_draw_quad(struct onedraw* r, float x0, float y0, float x1, float y2, od_quad_uv uv, uint8_t texture_index, draw_color srgb_color);
void od_draw_oriented_quad(struct onedraw* r, float x0, float y0, float x1, float y2, od_quad_uv uv, float angle, uint8_t texture_index, draw_color srgb_color);

#ifdef __cplusplus
}
#endif

#endif

