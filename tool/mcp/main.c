#include <stdio.h>
#include <math.h>

#define SOKOL_METAL
#include "sokol_app.h"

#include "mcp.h"

struct onedraw* renderer;
mcp_scene scene;
mcp_shot shot;

// The onedraw state buffer (and the later JSON-RPC buffers, T6) ; the shot has its own
// arena because it is rewound per capture
Arena arena;

#define FROM_HTML(html)   ((html&0xff)<<16) | ((html>>16)&0xff) | (html&0x00ff00) | 0xff000000

static const uint32_t miya_black = FROM_HTML(0x232228);
static const uint32_t miya_blue = FROM_HTML(0x2485a6);
static const uint32_t miya_red = FROM_HTML(0xc65046);
static const uint32_t miya_green = FROM_HTML(0x55a058);

// stdout carries the MCP protocol (later tasks), all logs go to stderr
void mcp_log(const char* string)
{
    fprintf(stderr, "%s\n", string);
}

// ---------------------------------------------------------------------------------------------------------------------------
void init(void)
{
    sapp_environment env = sapp_get_environment();

    renderer = od_init( &(onedraw_def)
    {
        .metal_device = (void*)env.metal.device,
        .preallocated_buffer = arena_alloc(&arena, od_min_memory_size()),
        .viewport_width = (uint32_t) sapp_width(),
        .viewport_height = (uint32_t) sapp_height(),
        .log_func = mcp_log,
        .srgb_backbuffer = false,
        .allow_screenshot = true,
        .high_dpi = sapp_high_dpi(),
        .cache = true
    });

    if (renderer == NULL)
        return;

    // static demo scene (T3), replayed every frame through the fit transform
    mcp_scene_init(&scene);
    mcp_shot_init(&shot);
    mcp_scene_append(&scene, &(mcp_op) { .type = MCP_OP_BOX, .f = { 102, 115, 461, 422, 16 }, .color = miya_blue });
    mcp_scene_append(&scene, &(mcp_op) { .type = MCP_OP_DISC, .f = { 666, 269, 115 }, .color = miya_red });
    mcp_scene_append(&scene, &(mcp_op) { .type = MCP_OP_RING, .f = { 358, 576, 77, 12 }, .color = miya_green });
    mcp_scene_append(&scene, &(mcp_op) { .type = MCP_OP_TEXT, .f = { 102, 500 }, .text = mcp_scene_intern(&scene, "onedraw mcp"), .color = miya_black });
}

// Temporary consumer for T5 : T6 (mcp.c) replaces it with the JSON-RPC image response
static void mcp_shot_consume_result(void)
{
    if (!shot.result.valid)
        return;

    if (shot.result.ok)
        fprintf(stderr, "screenshot : %ux%u px, %zu bytes of png (%zu base64)\n",
                shot.result.width, shot.result.height, (size_t) shot.result.png.count, (size_t) shot.result.b64.count);
    else
        fprintf(stderr, "screenshot : failed, the framebuffer is %ux%u px\n", shot.result.width, shot.result.height);

    shot.result.valid = false;
}

// ---------------------------------------------------------------------------------------------------------------------------
void frame(void)
{
    if (renderer == NULL)
        return;

    // (T6) the stdin drain lands here, before the window resize : a resize requested this
    // frame must land in the RESIZED event (od_resize + swapchain pool rebuild) before
    // sapp_acquire_swapchain(), which returns a drawable of the *current* size, or
    // od_end_frame would draw into a stale-size drawable (onedraw size assert)

    mcp_capture cap;
    bool capture_due = mcp_shot_tick(&shot, scene.canvas_w, scene.canvas_h,
                                     (uint32_t) sapp_width(), (uint32_t) sapp_height(), &cap);

    // fully obscured window : macOS provides no drawable (the nil drawable reads as a 0x0
    // swapchain and would fail onedraw's size assert), so skip the frame until visible again
    // (a pending capture waits ; mcp_shot_end still resolves error results made in the tick)
    sapp_swapchain sc = sapp_acquire_swapchain();
    if (sc.invalid)
    {
        mcp_shot_end(&shot);
        mcp_shot_consume_result();
        return;
    }

    mcp_transform t = mcp_fit_transform(scene.canvas_w, scene.canvas_h, (float) sapp_width(), (float) sapp_height());
    if (capture_due)
        cap.scale = t.scale;

    od_begin_frame(renderer);

    od_set_clear_color(renderer, scene.background);
    // the 2x atlas (high_dpi) is sampled at native size, so the font scale follows the
    // point->pixel scale (the 48px glyphs at 2x are sized for a 2.0 scale)
    od_set_font_scale(renderer, t.scale / 2.0f);

    mcp_scene_replay(renderer, &scene, t);

    if (capture_due)
    {
        uint8_t* buf = mcp_shot_prepare(&shot, &cap);
        if (buf != NULL)
        {
            od_set_capture_region(renderer, cap.x, cap.y, cap.w, cap.h);
            od_take_screenshot(renderer, buf);
        }
    }

    od_end_frame(renderer, (void*)sc.metal.current_drawable);

    mcp_shot_end(&shot);
    mcp_shot_consume_result();
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
        if (event->key_code == SAPP_KEYCODE_Q && event->modifiers == SAPP_MODIFIER_SUPER)
            sapp_quit();
        break;

    default: break;
    }
}

// ---------------------------------------------------------------------------------------------------------------------------
void cleanup(void)
{
    if (renderer != NULL)
    {
        od_terminate(renderer);
        renderer = NULL;
    }

    // renderer points into the arena (placement-new in the preallocated buffer)
    arena_free(&arena);
    arena_free(&shot.arena);
}

// ---------------------------------------------------------------------------------------------------------------------------
sapp_desc sokol_main(int argc, char* argv[])
{
    (void) argc;
    (void) argv;

    return (sapp_desc)
    {
        .width = MCP_WINDOW_W,
        .height = MCP_WINDOW_H,
        .init_cb = init,
        .frame_cb = frame,
        .cleanup_cb = cleanup,
        .event_cb = event,
        .high_dpi = true,
        .window_title = "onedraw mcp"
    };
}
