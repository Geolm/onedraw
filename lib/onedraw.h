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
    float u0, v0;   // top-left uv
    float u1, v1;   // bottom-right uv;
} od_quad_uv;

typedef struct od_stats
{
    uint32_t frame_index;
    uint32_t num_draw_cmd;
    uint32_t peak_num_draw_cmd;
    size_t gpu_memory_usage;
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
    } atlas;

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
// ==>  as the lib allocates gpu buffers, the total memory usage is higher than this and depends on resolution
//      use od_stats() to know the gpu memory usage
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
//          [num_slices]        must be <= 256. each quad can use a specific slice. 
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
// Makes the renderer copy the framebuffer into a cpu buffer when calling od_end_frame()
// This function must be called before between begin_frame/end_frame.
//      [out_pixels]            buffer of size (width*height*4), must be valid until od_end_frame() is called
// 
// Warning : the MTK::View has to have the flag framebufferOnly = FALSE;
//           by default GLFW the flag is set false *but* sokol_app set it to true (it's a crash in that case)
void od_take_screenshot(struct onedraw* r, void* out_pixels);

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
// Sets the clip rectangle, you set a mixumum of 256 clip shapes per frame
//      [min_x, min_y, max_x, max_y]    Coordinates of the clip rect
void od_set_cliprect(struct onedraw* r, float min_x, float min_y, float max_x, float max_y);

//-----------------------------------------------------------------------------------------------------------------------------
// Sets the clip disc, you set a mixumum of 256 clip shapes per frame
//      [cx, cy]        center of the disc
//      [radius]        radius of the disc
void od_set_clipdisc(struct onedraw* r, float cx, float cy, float radius);

//-----------------------------------------------------------------------------------------------------------------------------
// Outputs a blue color as the background of each tile. Mainly use to debug binning.
void od_set_culling_debug(struct onedraw* r, bool b);

//-----------------------------------------------------------------------------------------------------------------------------
// Begins a group
//      [smoothblend]       if true, [smooth_value] will be used for smoothmin
//      [smooth_value]      >= 0.f
//      [outline_width]     if equals to zero = no outline
void od_begin_group(struct onedraw* r, bool smoothblend, float smooth_value, float outline_width);

//-----------------------------------------------------------------------------------------------------------------------------
//  Ends a group
//      [outline_color]     color of the outline
void od_end_group(struct onedraw* r, draw_color outline_color);

//-----------------------------------------------------------------------------------------------------------------------------
// Returns the height of the font in pixels
float od_text_height(struct onedraw* r);

//-----------------------------------------------------------------------------------------------------------------------------
// Returns the width of the string [text] in pixels
float od_text_width(struct onedraw* r, const char* text);

//-----------------------------------------------------------------------------------------------------------------------------
// Draws a ring (circle)
//      [cx, cy]        center of the disc
//      [radius]
//      [thickness]
void od_draw_ring(struct onedraw* r, float cx, float cy, float radius, float thickness, draw_color srgb_color);

//-----------------------------------------------------------------------------------------------------------------------------
// Draws a disc
//      [cx, cy]        center of the disc
//      [radius]
void od_draw_disc(struct onedraw* r, float cx, float cy, float radius, draw_color srgb_color);

//-----------------------------------------------------------------------------------------------------------------------------
// Draws a disc, with a radial gradient
//      [cx, cy]        center of the disc
//      [radius]
//      [outter_color]  color on the edge of the disc
//      [inner_color]   color at the center of the disc
void od_draw_disc_gradient(struct onedraw* r, float cx, float cy, float radius, draw_color outter_color, draw_color inner_color);

//-----------------------------------------------------------------------------------------------------------------------------
// Draws a oriented box (square line segment)
//      [ax, bx], [by, cy]      line coordinates
//      [width]                 width of the box
//      [roundness]             rounded corners radius
void od_draw_oriented_box(struct onedraw* r, float ax, float ay, float bx, float by, float width, float roundness, draw_color srgb_color);


//-----------------------------------------------------------------------------------------------------------------------------
// Draws a oriented rectangle
//      [ax, bx], [by, cy]      line coordinates
//      [width]                 width of the box
//      [roundness]             rounded corners radius
//      [thickness]
void od_draw_oriented_rect(struct onedraw* r, float ax, float ay, float bx, float by, float width, float roundness, float thickness, draw_color srgb_color);

//-----------------------------------------------------------------------------------------------------------------------------
// Draws a line (sharp endpoints)
void od_draw_line(struct onedraw* r, float ax, float ay, float bx, float by, float width, draw_color srgb_color);

//-----------------------------------------------------------------------------------------------------------------------------
// Draws a capsule (rounded endpoints)
void od_draw_capsule(struct onedraw* r, float ax, float ay, float bx, float by, float radius, draw_color srgb_color);

//-----------------------------------------------------------------------------------------------------------------------------
// Draws a oriented ellipse
//      [ax, bx], [by, cy]      main axis of the ellipse
//      [width]                 width of the ellipse
void od_draw_ellipse(struct onedraw* r, float ax, float ay, float bx, float by, float width, draw_color srgb_color);

//-----------------------------------------------------------------------------------------------------------------------------
// Draws a oriented ellipse ring
//      [ax, bx], [by, cy]      main axis of the ellipse
//      [width]                 width of the ellipse
//      [thickness]
void od_draw_ellipse_ring(struct onedraw* r, float ax, float ay, float bx, float by, float width, float thickness, draw_color srgb_color);

//-----------------------------------------------------------------------------------------------------------------------------
// Draws a triangle
//      [vertices]              pointer to 6 floats, the 3 vertices coordinates (xy)
//      [roundness]             rounded corners radius
void od_draw_triangle(struct onedraw* r, const float* vertices, float roundness, draw_color srgb_color);

//-----------------------------------------------------------------------------------------------------------------------------
// Draws a triangle
//      [vertices]              pointer to 6 floats, the 3 vertices coordinates (xy)
//      [roundness]             rounded corners radius
//      [thickness]
void od_draw_triangle_ring(struct onedraw* r, const float* vertices, float roundness, float thickness, draw_color srgb_color);

//-----------------------------------------------------------------------------------------------------------------------------
// Draws a sector
//      [cx, cy]                center of the sector
//      [radius]                rounded corners radius
//      [start_angle]
//      [sweep_angle]
void od_draw_sector(struct onedraw* r, float cx, float cy, float radius, float start_angle, float sweep_angle, draw_color srgb_color);

//-----------------------------------------------------------------------------------------------------------------------------
// Draws a sector ring
//      [cx, cy]                center of the sector
//      [radius]                rounded corners radius
//      [start_angle]
//      [sweep_angle]
//      [thickness]
void od_draw_sector_ring(struct onedraw* r, float cx, float cy, float radius, float start_angle, float sweep_angle, float thickness, draw_color srgb_color);

//-----------------------------------------------------------------------------------------------------------------------------
// Draws a arc
//      [cx, cy]                center of the sector
//      [dx, dy]                direction of the arc
//      [aperture]              
//      [radius]
//      [thickness]
void od_draw_arc(struct onedraw* r, float cx, float cy, float dx, float dy, float aperture, float radius, float thickness, draw_color srgb_color);

//-----------------------------------------------------------------------------------------------------------------------------
// Draws a box
//      [x0, y0, x1, y1]        min, max coordinates of the box
//      [radius]                radius of the rounded corner, rounded corners are entirely within the rectangle’s bounds
void od_draw_box(struct onedraw* r, float x0, float y0, float x1, float y1, float radius, draw_color srgb_color);

//-----------------------------------------------------------------------------------------------------------------------------
// Draws a gaussian blurred box
//      [cx, cy]                    center of the box
//      [width, height]             size of the box
void od_draw_blurred_box(struct onedraw* r, float cx, float cy, float half_width, float half_height, float roundness, draw_color srgb_color);

//-----------------------------------------------------------------------------------------------------------------------------
// Draws one character
//      [x, y]                  top-left coordinates of the character
//      [c]                     only [32-126] char are supported
void od_draw_char(struct onedraw* r, float x, float y, char c, draw_color srgb_color);

//-----------------------------------------------------------------------------------------------------------------------------
// Draws a zero-terminated string, carriage return (\n) are taken in account
//      [x, y]                  top-left coordinates of the string
//      [text]                  string to be rendered
void od_draw_text(struct onedraw* r, float x, float y, const char* text, draw_color srgb_color);

//-----------------------------------------------------------------------------------------------------------------------------
// Draws a textured quad
//      [x0, y0, x1, y1]        min/max coordinates of the quad
//      [uv]                    top-left and bottom right uv, must be [0; 1], currently tiling/mirror not supported 
//      [slide_index]           index of the texture in the array
//      [srgb_color]            color that will multiply by the texture's fragment
void od_draw_quad(struct onedraw* r, float x0, float y0, float x1, float y1, od_quad_uv uv, uint32_t slice_index, draw_color srgb_color);

//-----------------------------------------------------------------------------------------------------------------------------
// Draws a oriented textured quad
//      [cx, cy]                center of the quad
//      [width, height]         size of the quad
//      [angle]                 angle in radians
//      [uv]                    top-left and bottom right uv, must be [0; 1]
void od_draw_oriented_quad(struct onedraw* r, float cx, float cy, float width, float height, float angle, od_quad_uv uv, uint32_t slice_index, draw_color srgb_color);

//-----------------------------------------------------------------------------------------------------------------------------
// Draws a quadratic bezier curve using adaptative tesselation
//      [control_points]        an array of 6 floats that represent the control points coordinates (x, y)
//      [width]
// Returns the number of capsules used or UINT32_MAX if the tesselation failed somehow
uint32_t od_draw_quadratic_bezier(struct onedraw* r, const float* control_points, float width, draw_color srgb_color);


//-----------------------------------------------------------------------------------------------------------------------------
// Draws a cubic bezier curve using adaptative tesselation
uint32_t od_draw_cubic_bezier(struct onedraw* r, const float* control_points, float width, draw_color srgb_color);

#ifdef __cplusplus
}
#endif

#endif

