// examples/lean_ui.c
//
// A lean_ui window on top of onedraw : lean_ui manages the window and the
// widgets, onedraw provides the pixels through the renderer callbacks.

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define SOKOL_METAL
#include "sokol_app.h"

#include "../lib/onedraw.h"
#include "lean_ui.h"
#include "vmem.h"

#define COUNOF(a)   (sizeof(a) / sizeof((a)[0]))

#define UI_FONT_SIZE (16)

// colors are packed 0xAABBGGRR, the same format lean_ui passes to the renderer callbacks
#define BG_COLOR (0xff332924u)

static const char* level_names[] = { "Low", "Med", "High" };
#define NUM_LEVELS COUNOF(level_names)

//----------------------------------------------------------------------------------------------------------------------------
// app state

struct onedraw* renderer = NULL;
ui_context* ui = NULL;
struct linear_allocator memory;   // onedraw buffer + lean_ui context

struct ui_backend
{
    struct onedraw* r;
    float font_size;
};
static struct ui_backend backend;

uint32_t level = 1;
float value = 50.f;
float gain = 0.f;
float bias = 0.f;
bool enabled = true;
bool verbose = false;
uint32_t clicks = 0;

float mouse_x = 0.f;
float mouse_y = 0.f;
enum ui_button_state button_state = button_idle;

double last_time = 0.0;

//----------------------------------------------------------------------------------------------------------------------------
// lean_ui renderer callbacks -> onedraw, colors are passed through unchanged

static void ui_draw_box(float x, float y, float width, float height, float radius, uint32_t srgb_color, void* user)
{
    struct ui_backend* b = (struct ui_backend*)user;
    od_draw_box(b->r, x, y, x + width, y + height, radius, (draw_color)srgb_color);
}

// lean_ui starts every text line at the top of a row that is 1.5 * font_height tall ; the baked
// font's line box (ascent + descent + line gap, 18.9px for the 14px Satoshi) is taller than
// font_height, so center the line box in the row
static void ui_draw_text(float x, float y, const char* text, uint32_t srgb_color, void* user)
{
    struct ui_backend* b = (struct ui_backend*)user;
    float line_height = od_text_line_height(b->r, od_font_regular, b->font_size);
    od_draw_text(b->r, x, y + (b->font_size * 1.5f - line_height) * .5f, text, od_font_regular, b->font_size, (draw_color)srgb_color);
}

static void ui_draw_line(float x0, float y0, float x1, float y1, float width, uint32_t srgb_color, void* user)
{
    struct ui_backend* b = (struct ui_backend*)user;
    od_draw_line(b->r, x0, y0, x1, y1, width, (draw_color)srgb_color);
}

// lean_ui pushes one clip per window (ui_begin_window) and "resets" it to the full screen in
// ui_end_window() : the reset maps to a pop, the windows being the only clips of the frame
static void ui_set_clip_rect(uint16_t min_x, uint16_t min_y, uint16_t max_x, uint16_t max_y, void* user)
{
    struct ui_backend* b = (struct ui_backend*)user;
    if ((min_x == 0) && (min_y == 0) && (max_x == UINT16_MAX) && (max_y == UINT16_MAX))
        od_pop_clip(b->r);
    else
        od_canvas_cliprect(b->r, (float)min_x, (float)min_y, (float)max_x, (float)max_y);
}

static float ui_text_width(const char* text, void* user)
{
    struct ui_backend* b = (struct ui_backend*)user;
    return od_text_width(b->r, text, od_font_regular, b->font_size);
}

//----------------------------------------------------------------------------------------------------------------------------
static float frame_delta_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
    float dt = (float)(now - last_time);
    last_time = now;
    return (dt > 0.f) ? dt : (1.f / 60.f);
}

static void custom_log(const char* string)
{
    fprintf(stdout, "%s\n", string);
}

//----------------------------------------------------------------------------------------------------------------------------
static void draw_window(void)
{
    ui_begin_window(ui, "Widgets", 24.f, 24.f, 340.f, 380.f, window_resizable);

    ui_text(ui, align_left, "lean_ui widgets, drawn by onedraw");
    ui_newline(ui);
    ui_text(ui, align_left, "drag the title bar to move the window");
    ui_newline(ui);

    ui_separator(ui);

    ui_value(ui, "Level", "%s", level_names[level]);
    ui_segmented(ui, level_names, NUM_LEVELS, &level);

    ui_slider(ui, "Value", 0.f, 100.f, 1.f, &value, "%.0f");

    ui_knob(ui, "Gain", -10.f, 10.f, 0.f, &gain);
    ui_knob(ui, "Bias", -10.f, 10.f, 0.f, &bias);
    ui_newline(ui);

    ui_toggle(ui, "Enabled", &enabled);
    ui_toggle(ui, "Verbose", &verbose);

    ui_separator(ui);

    if (ui_button(ui, "Click me", align_center))
        clicks++;
    ui_newline(ui);

    ui_value(ui, "Clicks", "%u", clicks);

    ui_end_window(ui);
}

//----------------------------------------------------------------------------------------------------------------------------
// sokol_app callbacks

static void frame(void)
{
    if ((renderer == NULL) || (ui == NULL))
        return;

    // fully obscured window : macOS provides no drawable, skip the frame until visible
    sapp_swapchain sc = sapp_acquire_swapchain();
    if (sc.invalid)
        return;

    float dt = frame_delta_time();
    if (dt > 0.25f)
        dt = 0.25f;

    // sapp_width() is the framebuffer size in device pixels : divide by the dpi scale to get
    // window points, the canvas maps those points onto the framebuffer
    float scale = sapp_dpi_scale();
    float width = (float)sapp_width() / scale;
    float height = (float)sapp_height() / scale;

    // logical-points canvas : lean_ui works in the same units
    od_set_canvas(renderer, 0.f, 0.f, width, height);

    od_begin_frame(renderer);

    // sokol reports the mouse in device pixels, lean_ui works in the canvas's logical points
    ui_update_mouse_pos(ui, mouse_x / scale, mouse_y / scale);

    // lean_ui fires a widget on the frame where the button transitioned to pressed, so the
    // transition is fed once and then cleared
    if (button_state != button_idle)
    {
        ui_update_mouse_button(ui, button_state);
        button_state = button_idle;
    }

    ui_begin_frame(ui, dt);

    draw_window();

    ui_end_frame(ui);

    od_end_frame(renderer, (void*)sc.metal.current_drawable);
}

static void event(const sapp_event* e)
{
    if (renderer == NULL)
        return;

    // the event struct carries the current mouse position in every event
    mouse_x = e->mouse_x;
    mouse_y = e->mouse_y;

    switch (e->type)
    {
    case SAPP_EVENTTYPE_RESIZED:
        od_resize(renderer, (uint32_t)e->framebuffer_width, (uint32_t)e->framebuffer_height);
        break;

    case SAPP_EVENTTYPE_MOUSE_DOWN:
        button_state = button_pressed;
        break;

    case SAPP_EVENTTYPE_MOUSE_UP:
        button_state = button_released;
        break;

    case SAPP_EVENTTYPE_KEY_DOWN:
        if ((e->key_code == SAPP_KEYCODE_Q) && (e->modifiers & SAPP_MODIFIER_SUPER))
            sapp_quit();
        break;

    default:
        break;
    }
}

static void init(void)
{
    sapp_environment env = sapp_get_environment();

    // +32 : linear_alloc rounds each allocation up to a 16-byte boundary
    size_t total = od_min_memory_size() + ui_min_memory_size() + 32;
    if (!linear_init(&memory, total))
        return;

    void* od_buffer = linear_alloc(&memory, od_min_memory_size());
    renderer = od_init( &(onedraw_def)
    {
        .metal_device = (void*)env.metal.device,
        .preallocated_buffer = od_buffer,
        .viewport_width = (uint32_t)sapp_width(),
        .viewport_height = (uint32_t)sapp_height(),
        .log_func = custom_log,
        .srgb_backbuffer = false
    });

    if (renderer == NULL)
        return;

    od_set_clear_color(renderer, BG_COLOR);

    backend.r = renderer;
    backend.font_size = (float)UI_FONT_SIZE;

    void* ui_buffer = linear_alloc(&memory, ui_min_memory_size());
    ui = ui_init(&(ui_def)
    {
        .preallocated_buffer = ui_buffer,
        .renderer_callbacks =
        {
            .draw_box = ui_draw_box,
            .draw_text = ui_draw_text,
            .draw_line = ui_draw_line,
            .set_clip_rect = ui_set_clip_rect,
            .text_width = ui_text_width,
            .user = &backend
        },
        .font_height = (float)UI_FONT_SIZE
    });
}

static void cleanup(void)
{
    if (renderer != NULL)
    {
        od_terminate(renderer);
        renderer = NULL;
    }
    linear_free(&memory);
}

sapp_desc sokol_main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;

    return (sapp_desc)
    {
        .width = 1280,
        .height = 720,
        .init_cb = init,
        .frame_cb = frame,
        .cleanup_cb = cleanup,
        .event_cb = event,
        .high_dpi = true,
        .window_title = "lean_ui example"
    };
}
