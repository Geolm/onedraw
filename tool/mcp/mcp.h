/*

    onedraw mcp server — shared types (plan/mcp-server.md, tasks T3-T6)

*/

#ifndef MCP_H
#define MCP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "../../lib/onedraw.h"

//----------------------------------------------------------------------------------------------------------------------------
// Point -> pixel fit transform (plan §3)
//
// The canvas is a logical rectangle in points, the framebuffer in pixels. The transform maps
// canvas points to framebuffer pixels with the canvas centered (the letterbox is the clear color).
//----------------------------------------------------------------------------------------------------------------------------
typedef struct mcp_transform
{
    float scale;   // pixels per point
    float offx;    // framebuffer x of canvas point (0,0)
    float offy;    // framebuffer y of canvas point (0,0)
} mcp_transform;

// Computes the transform that fits the [canvas_w]x[canvas_h] point canvas centered in the
// [fb_w]x[fb_h] pixel framebuffer
mcp_transform mcp_fit_transform(float canvas_w, float canvas_h, float fb_w, float fb_h);

// Maps a canvas x / y coordinate or a size (radius, width...) through the transform
static inline float mcp_tx_x(mcp_transform t, float x) { return t.offx + x * t.scale; }
static inline float mcp_tx_y(mcp_transform t, float y) { return t.offy + y * t.scale; }
static inline float mcp_tx_s(mcp_transform t, float v) { return v * t.scale; }

//----------------------------------------------------------------------------------------------------------------------------
// Tool commands (the `cmd` field of a tools/call, plan §4)
//----------------------------------------------------------------------------------------------------------------------------
enum mcp_cmd
{
    MCP_CMD_CLEAR = 0,
    MCP_CMD_DRAW,
    MCP_CMD_UPDATE,
    MCP_CMD_REMOVE,
    MCP_CMD_SCREENSHOT
};

// Result of parsing a tools/call `arguments` object : on success the scene has already been
// mutated (clear / draw / update / remove), screenshot only fills [shot_w]/[shot_h]
typedef struct mcp_parse_result
{
    enum mcp_cmd cmd;
    uint32_t index;    // draw : first assigned index, update / remove : the target index
    uint32_t count;    // draw : number of ops appended
    uint32_t live;     // number of alive ops after the command
    uint32_t shot_w;   // screenshot : requested output pixels, 0 = omitted (canvas rect)
    uint32_t shot_h;
} mcp_parse_result;

//----------------------------------------------------------------------------------------------------------------------------
// Scene (plan §4, D4)
//
// A flat, ordered, index-addressable list of draw ops. `draw` appends an op and returns its
// stable index, `update` replaces the op in place (even its type), `remove` tombstones it —
// indices are never renumbered. Text ops point into the scene string pool (mcp_scene_intern).
// All coordinates and sizes are in canvas points.
//----------------------------------------------------------------------------------------------------------------------------

// An op type, mirrors the tool `type` field (plan §6)
enum mcp_op_type
{
    MCP_OP_DISC = 0,
    MCP_OP_RING,
    MCP_OP_DISC_GRADIENT,
    MCP_OP_BOX,
    MCP_OP_BLURRED_BOX,
    MCP_OP_LINE,
    MCP_OP_CAPSULE,
    MCP_OP_CAPSULE_GRADIENT,
    MCP_OP_ORIENTED_BOX,
    MCP_OP_ORIENTED_RECT,
    MCP_OP_ELLIPSE,
    MCP_OP_ELLIPSE_RING,
    MCP_OP_TRIANGLE,
    MCP_OP_TRIANGLE_RING,
    MCP_OP_SECTOR,
    MCP_OP_SECTOR_RING,
    MCP_OP_ARC,
    MCP_OP_QUADRATIC_BEZIER,
    MCP_OP_CUBIC_BEZIER,
    MCP_OP_CHAR,
    MCP_OP_TEXT,
    MCP_OP_DASHED_LINE,
    MCP_OP_DASHED_RING,
    MCP_OP_DASHED_BOX,
    MCP_OP_DASHED_ORIENTED_BOX,
    MCP_OP_DASHED_ELLIPSE_RING,
    MCP_OP_DASHED_TRIANGLE_RING,
    MCP_OP_BEGIN_GROUP,
    MCP_OP_END_GROUP,
    MCP_OP_PUSH_CLIPRECT,
    MCP_OP_PUSH_CLIPRECT_ROUNDED,
    MCP_OP_PUSH_CLIPDISC,
    MCP_OP_POP_CLIP
};

// A single draw op, [f] slots are interpreted per type (see mcp_scene_replay)
typedef struct mcp_op
{
    uint32_t index;        // stable index, set by the scene (== position in the op array)
    enum mcp_op_type type;
    bool alive;            // false = tombstoned by mcp_scene_remove
    bool smoothblend;      // begin_group only
    draw_color color;      // main color (end_group: the outline color)
    draw_color color2;     // second color (gradients: disc inner, capsule secondary)
    float f[10];           // numeric parameters, in canvas points
    od_dash_style dash;    // dashed ops
    char c;                // MCP_OP_CHAR
    const char* text;      // MCP_OP_TEXT, points into the scene string pool
} mcp_op;

typedef struct mcp_scene
{
    mcp_op* ops;
    uint32_t count;        // ops, alive or not (removed ops keep their slot)
    uint32_t capacity;
    char* text;            // string pool
    size_t text_len;
    size_t text_capacity;
    float canvas_w;        // canvas width in points
    float canvas_h;        // canvas height in points
    draw_color background; // clear color
} mcp_scene;

// Initializes with the default 1024x768 point canvas (plan D8) and a light background
void mcp_scene_init(mcp_scene* s);

// Wipes all ops and sets the canvas (the `clear` command)
void mcp_scene_reset(mcp_scene* s, float canvas_w, float canvas_h, draw_color background);

// Appends one op, returns the assigned stable index or UINT32_MAX on allocation failure
uint32_t mcp_scene_append(mcp_scene* s, const mcp_op* op);

// Replaces the op at [index] in place (the type may change), false if [index] is out of range or a hole
bool mcp_scene_update(mcp_scene* s, uint32_t index, const mcp_op* op);

// Tombstones the op at [index] (leaves a stable hole), false if [index] is out of range or a hole
bool mcp_scene_remove(mcp_scene* s, uint32_t index);

// Number of alive ops
uint32_t mcp_scene_live_count(const mcp_scene* s);

// Copies the NUL-terminated [str] into the string pool and returns a pointer valid until the
// buffer is reallocated, NULL on allocation failure
const char* mcp_scene_intern(mcp_scene* s, const char* str);

// Like mcp_scene_intern for a string that isn't NUL-terminated (e.g. a parsed JSON value)
const char* mcp_scene_intern_len(mcp_scene* s, const char* data, size_t len);

// Returns false if the alive ops have unbalanced groups or clips
bool mcp_scene_is_balanced(const mcp_scene* s);

// Replays all alive ops through the onedraw API, applying the point->pixel transform
// (od_set_font_scale must have been set by the caller first). Returns false and draws nothing
// if the scene is unbalanced.
//
// [f] slots per type :
//   disc                  0 cx, 1 cy, 2 radius
//   ring                  0 cx, 1 cy, 2 radius, 3 thickness
//   disc_gradient         0 cx, 1 cy, 2 radius            (color = outer, color2 = inner)
//   box                   0 x0, 1 y0, 2 x1, 3 y1, 4 radius
//   blurred_box           0 cx, 1 cy, 2 half_width, 3 half_height, 4 roundness
//   line                  0 ax, 1 ay, 2 bx, 3 by, 4 width
//   capsule               0 ax, 1 ay, 2 bx, 3 by, 4 radius
//   capsule_gradient      0 ax, 1 ay, 2 bx, 3 by, 4 radius (color = primary, color2 = secondary)
//   oriented_box          0 ax, 1 ay, 2 bx, 3 by, 4 width, 5 roundness
//   oriented_rect         0 ax, 1 ay, 2 bx, 3 by, 4 width, 5 roundness, 6 thickness
//   ellipse               0 ax, 1 ay, 2 bx, 3 by, 4 width
//   ellipse_ring          0 ax, 1 ay, 2 bx, 3 by, 4 width, 5 thickness
//   triangle              0..5 vertices, 6 roundness
//   triangle_ring         0..5 vertices, 6 roundness, 7 thickness
//   sector                0 cx, 1 cy, 2 radius, 3 start_angle, 4 sweep_angle
//   sector_ring           0 cx, 1 cy, 2 radius, 3 start_angle, 4 sweep_angle, 5 thickness
//   arc                   0 cx, 1 cy, 2 dx, 3 dy, 4 aperture, 5 radius, 6 thickness (dx,dy not transformed)
//   quadratic_bezier      0..5 control points, 6 width
//   cubic_bezier          0..7 control points, 8 width
//   char                  0 x, 1 y (+ c)
//   text                  0 x, 1 y (+ text)
//   dashed_line           0 ax, 1 ay, 2 bx, 3 by, 4 width (+ dash)
//   dashed_ring           0 cx, 1 cy, 2 radius, 3 thickness (+ dash)
//   dashed_box            0 x0, 1 y0, 2 x1, 3 y1, 4 radius, 5 thickness (+ dash)
//   dashed_oriented_box   0 ax, 1 ay, 2 bx, 3 by, 4 width, 5 roundness, 6 thickness (+ dash)
//   dashed_ellipse_ring   0 ax, 1 ay, 2 bx, 3 by, 4 width, 5 thickness (+ dash)
//   dashed_triangle_ring  0..5 vertices, 6 thickness (+ dash)
//   begin_group           0 smooth_value, 1 outline_width (+ smoothblend)
//   end_group             (color = outline color)
//   push_cliprect         0 min_x, 1 min_y, 2 max_x, 3 max_y
//   push_cliprect_rounded 0 min_x, 1 min_y, 2 max_x, 3 max_y, 4 radius
//   push_clipdisc         0 cx, 1 cy, 2 radius
//   pop_clip              -
bool mcp_scene_replay(struct onedraw* r, const mcp_scene* s, mcp_transform t);

//----------------------------------------------------------------------------------------------------------------------------
// Tool-JSON parsing (parse.c, sj.h + sv.h, plan §4/§6)
//
// [buf] must be writable (sj mutates the input) and NUL-terminated by the caller.
// On failure [err] is filled with a message and, for JSON errors, the line/col from
// sj_location; on success it is left untouched.
//----------------------------------------------------------------------------------------------------------------------------
// Parses a #rrggbb or #rrggbbaa string into a draw_color (A8R8G8B8, sRGB).
bool mcp_parse_color(const char* str, draw_color* out);

// Parses one tools/call `arguments` object and applies it to the scene (clear / draw /
// update / remove mutate the scene immediately, screenshot only fills the result)
bool mcp_parse_tool_call(mcp_scene* s, char* buf, size_t len, mcp_parse_result* out, char* err, size_t err_size);

#endif
