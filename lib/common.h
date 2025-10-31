#ifndef __COMMON_H__
#define __COMMON_H__

// ---------------------------------------------------------------------------------------------------------------------------
// This file is included by the cpp renderer and the shaders
// ---------------------------------------------------------------------------------------------------------------------------

// ---------------------------------------------------------------------------------------------------------------------------
// renderer constants
#define TILE_SIZE (16)
#define REGION_SIZE (16)
#define MAX_NODES_COUNT (1<<22)
#define INVALID_INDEX (0xffffffff)
#define MAX_CLIPS (256)
#define MAX_COMMANDS (1<<16)
#define MAX_DRAWDATA (MAX_COMMANDS * 4)
#define SIMD_GROUP_SIZE (32)
#define LAST_COMMAND (MAX_COMMANDS-1)
#define MAX_THREADS_PER_THREADGROUP (1024)
#define MAX_GLYPHS (128)

// ---------------------------------------------------------------------------------------------------------------------------
// cpp compatibility
#ifndef __METAL_VERSION__
#pragma once
#define constant
#define atomic_uint uint32_t
#define device
#define command_buffer void*
#define texture_half uint64_t
#ifdef __cplusplus
    typedef struct alignas(8) {float x, y;} float2;
    typedef struct alignas(16) {float x, y, z, w;} float4;
#else
    typedef struct {float x, y;} float2;
    typedef struct {float x, y, z, w;} float4;
#endif
#else
using namespace metal;
#define texture_half texture2d<half>
#endif

// packed on 6 bits
enum command_type
{
    primitive_char = 0,
    primitive_aabox = 1,
    primitive_oriented_box = 2,
    primitive_disc = 3,
    primitive_triangle = 4,
    primitive_ellipse = 5,
    primitive_pie = 6,
    primitive_arc = 7,
    primitive_blurred_box = 8,
    primitive_quad = 9,
    
    begin_group = 32,
    end_group = 33
};

enum primitive_fillmode
{
    fill_solid = 0,
    fill_outline = 1,
    fill_hollow = 2,
    fill_last = 3
};

#define COMMAND_TYPE_MASK   (0x3f)
#define PRIMITIVE_FILLMODE_MASK (0xC0)
#define PRIMITIVE_FILLMODE_SHIFT (6)


enum sdf_operator
{
    op_overwrite = 0,
    op_blend = 1
};

// packed 8 bytes structure
typedef struct draw_command
{
    uint32_t data_index;
#ifdef RASTERIZER_SHADER
    uint32_t packed_data;
#else
    uint8_t extra;
    uint8_t clip_index;
    uint8_t fillmode;
    uint8_t type;
#endif
} draw_command;

typedef struct tile_node
{
    uint32_t next;
    uint16_t command_index;
    uint8_t command_type;
    uint8_t padding;
} tile_node;

typedef struct counters
{
    atomic_uint num_nodes;
    atomic_uint num_tiles;
    uint32_t pad[2];
} counters;

typedef struct clip_rect
{
    float min_x, min_y, max_x, max_y;
} clip_rect;

typedef struct quantized_aabb
{
#ifdef RASTERIZER_SHADER
    uint32_t packed_data;
#else
    uint8_t min_x;
    uint8_t min_y;
    uint8_t max_x;
    uint8_t max_y;
#endif
} quantized_aabb;

typedef struct font_char
{
    float2 uv_topleft;
    float2 uv_bottomright;
    float width;
    float height;
} font_char;

typedef struct draw_cmd_arguments
{
    constant draw_command* commands;
    constant uint32_t* colors;
    constant quantized_aabb* commands_aabb;
    constant float* draw_data;
    constant clip_rect* clips;
    constant font_char* glyphs;
    texture_half font;
    float4 clear_color;
    uint32_t num_commands;
    uint32_t max_nodes;
    uint32_t num_tile_width;
    uint32_t num_tile_height;
    uint32_t num_region_width;
    uint32_t num_region_height;
    uint32_t num_groups;
    float aa_width;
    float2 screen_div;
    uint32_t num_elements_per_thread;
    bool culling_debug;
    bool srgb_backbuffer;
} draw_cmd_arguments;

typedef struct tiles_data
{
    device uint32_t* head;
    device tile_node* nodes;
    device uint16_t* tile_indices;
} tiles_data;

typedef struct output_command_buffer
{
    command_buffer cmd_buffer;
} output_command_buffer;

#ifdef __METAL_VERSION__
inline float2 skew(float2 v) {return float2(-v.y, v.x);}
inline float cross2(float2 a, float2 b ) {return a.x*b.y - a.y*b.x;}

template<typename T>
T linearstep(T edge0, T edge1, T x)
{
    return clamp((x - edge0) / (edge1 - edge0), T(0), T(1));
}
#endif

// ---------------------------------------------------------------------------------------------------------------------------
// cpp compatibility
#ifndef __METAL_VERSION__
#undef constant
#undef atomic_uint
#undef device
#undef command_buffer
#endif


#endif