#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#define SOKOL_METAL
#include "sokol_app.h"

#include "mcp.h"

// tool/mcp/window.mm (AppKit shim, see that file)
void mcp_set_window_points(int w, int h);

struct onedraw* renderer;
mcp_scene scene;

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
        .preallocated_buffer = malloc(od_min_memory_size()),
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
    mcp_scene_append(&scene, &(mcp_op) { .type = MCP_OP_BOX, .f = { 102, 115, 461, 422, 16 }, .color = miya_blue });
    mcp_scene_append(&scene, &(mcp_op) { .type = MCP_OP_DISC, .f = { 666, 269, 115 }, .color = miya_red });
    mcp_scene_append(&scene, &(mcp_op) { .type = MCP_OP_RING, .f = { 358, 576, 77, 12 }, .color = miya_green });
    mcp_scene_append(&scene, &(mcp_op) { .type = MCP_OP_TEXT, .f = { 102, 500 }, .text = mcp_scene_intern(&scene, "onedraw mcp"), .color = miya_black });
}

// ---------------------------------------------------------------------------------------------------------------------------
void frame(void)
{
    if (renderer == NULL)
        return;

    // the stdin-driven window resize (T5) must run before sapp_acquire_swapchain() : that call
    // returns a drawable of the *current* size, so a resize requested this frame has to land in
    // the RESIZED event (od_resize + swapchain pool rebuild) before the next drawable is pulled,
    // or od_end_frame would draw into a stale-size drawable (onedraw size assert)

    // fully obscured window : macOS provides no drawable (the nil drawable reads as a 0x0
    // swapchain and would fail onedraw's size assert), so skip the frame until visible again
    sapp_swapchain sc = sapp_acquire_swapchain();
    if (sc.invalid)
        return;

    mcp_transform t = mcp_fit_transform(scene.canvas_w, scene.canvas_h, sapp_widthf(), sapp_heightf());

    od_begin_frame(renderer);

    od_set_clear_color(renderer, scene.background);
    // the 2x atlas (high_dpi) is sampled at native size, so the font scale follows the
    // point->pixel scale (the 48px glyphs at 2x are sized for a 2.0 scale)
    od_set_font_scale(renderer, t.scale / 2.0f);

    mcp_scene_replay(renderer, &scene, t);

    od_end_frame(renderer, (void*)sc.metal.current_drawable);
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
        free(renderer);
        renderer = NULL;
    }
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
        .event_cb = event,
        .high_dpi = true,
        .window_title = "onedraw mcp"
    };
}
