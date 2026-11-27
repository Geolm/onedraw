// examples/lean_ui.c
//
// A lean_ui window on top of onedraw : lean_ui manages the window and the
// widgets, onedraw provides the pixels through the renderer callbacks.

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <math.h>

#define SOKOL_METAL
#include "sokol_app.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define FLAG_IMPLEMENTATION
#include "flag.h"

#include "../lib/onedraw.h"
#include "lean_ui.h"
#include "mjpegw.h"
#include "vmem.h"

#define COUNOF(a)   (sizeof(a) / sizeof((a)[0]))

#define UI_FONT_SIZE (16)

// colors are packed 0xAABBGGRR, the same format lean_ui passes to the renderer callbacks
#define BG_COLOR (0xffffffff)

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

static bool do_screenshot = false;
static bool do_record_video = false;
static char* record_output = "example.avi";
static struct mjpegw_context* record_ctx = NULL;
static uint8_t* record_pixels = NULL;
static uint32_t record_region_w = 0;
static uint32_t record_region_h = 0;
static uint32_t record_frame_count = 0;

// the Widgets window geometry (logical points); the recording capture region is derived from it
#define WIDGETS_WINDOW_X (24.f)
#define WIDGETS_WINDOW_Y (24.f)
#define WIDGETS_WINDOW_W (340.f)
#define WIDGETS_WINDOW_H (600.f)

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
static void ui_draw_text(float x, float y, const char* text, size_t length, uint32_t srgb_color, void* user)
{
    struct ui_backend* b = (struct ui_backend*)user;
    float line_height = od_text_line_height(b->r, od_font_regular, b->font_size);
    od_draw_string_view(b->r, x, y + (b->font_size * 1.5f - line_height) * .5f, text, length, od_font_regular, b->font_size, (draw_color)srgb_color);
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

static float ui_text_width(const char* text, size_t length, void* user)
{
    struct ui_backend* b = (struct ui_backend*)user;
    return od_string_view_width(b->r, text, length, od_font_regular, b->font_size);
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
static void draw_window(float dt)
{
    ui_begin_window(ui, "Widgets", WIDGETS_WINDOW_X, WIDGETS_WINDOW_Y, WIDGETS_WINDOW_W, WIDGETS_WINDOW_H, window_resizable);

    ui_text(ui, align_left, "On the left");
    ui_text(ui, align_right, "On the right");
    ui_newline(ui);

    od_stats stats;
    od_get_stats(renderer, &stats);

    ui_value(ui, "gpu time", "%2.2f ms", stats.gpu_time_ms);
    ui_value(ui, "delta time", "%2.2f ms", dt * 1000.f);
    ui_text(ui, align_center, "In the middle");
    ui_newline(ui);
    static bool test_bool, en_dessous;
    ui_toggle(ui, "This is a toggle", &test_bool);
    ui_toggle(ui, "and another one", &en_dessous);

    ui_separator(ui);
    ui_text(ui, align_center, "Choose your destiny");
    ui_newline(ui);
    const char* list[] = {"Novice", "Warrior", "Master", "Champion"};
    static uint32_t selected = 2;

    ui_segmented(ui, list, 4, &selected);
    ui_text(ui, align_left, "Your choice");
    ui_text(ui, align_center,  list[selected]);
    ui_newline(ui);
    ui_separator(ui);
    static float quantity = 5.f;
    ui_slider(ui, "Distance", 0.f, 100.f, 1.f, &quantity, "%3.2fkm");
    
    ui_separator(ui);
    ui_button(ui, "Left Button", align_left);
    ui_button(ui, "Center Button", align_center);
    ui_button(ui, "Right Button", align_right);

    ui_newline(ui);

    static float drive = 2.f, pan = 0.f, volume = .7f;
    ui_knob(ui, "drive", 1.f, 10.f, 2.f, &drive);
    ui_knob(ui, "pan", -1.f, 1.f, 0.f, &pan);
    ui_knob(ui, "volume", 0.f, 1.f, .7f, &volume);

    ui_newline(ui);
    ui_separator(ui);

    // oscillate the target so the fill ease is visible (shown in the label row instantly,
    // the bar catches up over a few frames)
    static float phase = 0.f;
    phase += dt;
    float upload_target = .55f + .45f * sinf(phase * .7f);
    ui_progress(ui, "Uploading", upload_target, "%.0f%%", upload_target * 100.f);

    // a static target : the bar starts already in place
    ui_progress(ui, "Storage", 18.4f / 20.f, "%.1f / %.0f GB", 18.4f, 20.f);

    static const char paragraph[] = "This paragraph is laid out by ui_paragraph, which wraps the text on spaces to the window's content width "
                                    "and re-flows it every frame.\n"
                                    "It also honors explicit line breaks, like this one. The thin bar in the right gutter is the scrollbar : "
                                    "it appears only because this content is taller than the window, and its height and position track "
                                    "the ratio of visible to total content. "
                                    "Scroll the wheel while the cursor is over the window, or grab the thumb and drag it, to move through the text. "
                                    "The bar itself is interactive too : it grows wider and more opaque while hovered, and the scroll position "
                                    "eases towards its target so the motion feels light rather than stepwise. "
                                    "Widgets that scroll under the title bar stop catching clicks, so scrolled-out content never stays interactive. "
                                    "Resize the window and the paragraph re-flows to the new width, while words longer than a line overflow "
                                    "and get clipped rather than broken mid-word.";
    ui_paragraph(ui, paragraph, strlen(paragraph));

    ui_end_window(ui);
}

static void draw_plain_window(void)
{
    ui_begin_window(ui, "No scrollbar", 400.f, 24.f, 300.f, 260.f, 0);

    ui_text(ui, align_left, "This content fits, so no scrollbar");
    ui_newline(ui);

    static bool flag = true;
    ui_toggle(ui, "a toggle", &flag);

    static float amount = 3.f;
    ui_slider(ui, "Amount", 0.f, 10.f, .5f, &amount, "%3.1f");

    static float level = .5f;
    ui_knob(ui, "level", 0.f, 1.f, .5f, &level);
    ui_button(ui, "Okay", align_right);

    ui_end_window(ui);
}

static void draw_borderless_window(void)
{
    ui_begin_window(ui, "Borderless", 724.f, 24.f, 300.f, 260.f, window_no_border);

    ui_text(ui, align_left, "No border ring : the window is");
    ui_newline(ui);
    ui_text(ui, align_left, "one rounded card, the title bar");
    ui_newline(ui);
    ui_text(ui, align_left, "spans the full width");
    ui_newline(ui);

    static bool flag = false;
    ui_toggle(ui, "a toggle", &flag);

    ui_end_window(ui);
}

static void draw_panel_window(void)
{
    ui_begin_window(ui, "Panel", 724.f, 308.f, 300.f, 150.f, window_no_chrome);

    ui_text(ui, align_left, "No chrome at all : a plain panel");
    ui_newline(ui);
    ui_text(ui, align_left, "that cannot be moved by dragging");
    ui_newline(ui);
    ui_button(ui, "Okay", align_right);
    ui_newline(ui);

    ui_end_window(ui);
}

//----------------------------------------------------------------------------------------------------------------------------
// sokol_app callbacks

static void frame(void)
{
    if ((renderer == NULL) || (ui == NULL))
        return;

    // fully obscured window : macOS provides no drawable, skip the frame until visible
    // (screenshot mode renders into the offscreen target and needs no drawable)
    sapp_swapchain sc = sapp_acquire_swapchain();
    if (sc.invalid && !do_screenshot)
        return;

    // recording : a fixed timestep so the video timeline is deterministic
    float dt;
    if (record_ctx != NULL)
        dt = 1.f / 60.f;
    else
    {
        dt = frame_delta_time();
        if (dt > 0.25f)
            dt = 0.25f;
    }

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

    draw_window(dt);
    draw_plain_window();
    draw_borderless_window();
    draw_panel_window();

    ui_end_frame(ui);

    static uint32_t num_frames = 0;
    ++num_frames;

    // -screenshot : capture the third frame to example_screenshot.png and quit
    uint8_t* pixels = (do_screenshot && (num_frames == 3)) ? malloc((size_t)sapp_width() * sapp_height() * 4) : NULL;
    if (pixels != NULL)
        od_take_screenshot(renderer, pixels);
    if (record_ctx != NULL)
        od_take_screenshot(renderer, record_pixels);

    od_end_frame(renderer, do_screenshot ? NULL : (void*)sc.metal.current_drawable);

    if (pixels != NULL)
    {
        // the gpu pixels are BGRA, the png wants RGBA
        for (uint32_t i = 0; i < (uint32_t)sapp_width() * sapp_height(); ++i)
        {
            uint8_t b = pixels[4*i];
            pixels[4*i] = pixels[4*i + 2];
            pixels[4*i + 2] = b;
        }
        stbi_write_png("example_screenshot.png", sapp_width(), sapp_height(), 4, pixels, sapp_width() * sizeof(uint32_t));
        free(pixels);
        sapp_quit();
    }

    // recording : same capture, encoded as a video frame (BGRA -> RGBA like the png path)
    if (record_ctx != NULL)
    {
        for (uint32_t i = 0; i < record_region_w * record_region_h; ++i)
        {
            uint8_t b = record_pixels[4*i];
            record_pixels[4*i] = record_pixels[4*i + 2];
            record_pixels[4*i + 2] = b;
        }
        mjpegw_add_frame(record_ctx, record_pixels, 2);
        ++record_frame_count;
    }
}

// finalize and free the video; safe to call when nothing is recording
static void close_recording(void)
{
    if (record_ctx != NULL)
    {
        printf("video written: %s (%u frames @ 60 fps)\n", record_output, record_frame_count);
        mjpegw_close(record_ctx);
        record_ctx = NULL;
    }
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

    case SAPP_EVENTTYPE_MOUSE_SCROLL:
        ui_update_mouse_wheel(ui, e->scroll_y);
        break;

    case SAPP_EVENTTYPE_KEY_DOWN:
        if ((e->key_code == SAPP_KEYCODE_Q) && (e->modifiers & SAPP_MODIFIER_SUPER))
        {
            close_recording();
            sapp_quit();
        }
        break;

    case SAPP_EVENTTYPE_QUIT_REQUESTED:
        // let the default quit proceed; just finalize the video first
        close_recording();
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
        .srgb_backbuffer = false,
        .allow_screenshot = do_screenshot || do_record_video,
        .allow_offscreen = do_screenshot
    });

    if (renderer == NULL)
        return;

    // recording the Widgets window : the capture region is fixed at the window's initial geometry,
    // and the recorded pixels are reused frame to frame
    if (do_record_video)
    {
        float scale = sapp_dpi_scale();
        uint32_t region_x = (uint32_t)(WIDGETS_WINDOW_X * scale + .5f);
        uint32_t region_y = (uint32_t)(WIDGETS_WINDOW_Y * scale + .5f);
        uint32_t region_w = (uint32_t)(WIDGETS_WINDOW_W * scale + .5f);
        uint32_t region_h = (uint32_t)(WIDGETS_WINDOW_H * scale + .5f);

        record_pixels = malloc((size_t)region_w * region_h * 4);
        if (record_pixels == NULL)
        {
            fprintf(stderr, "error: could not allocate the video capture buffer (%ux%u)\n", region_w, region_h);
            do_record_video = false;
        }
        else
        {
            record_region_w = region_w;
            record_region_h = region_h;
            od_set_capture_region(renderer, region_x, region_y, region_w, region_h);

            record_ctx = mjpegw_open(record_output, region_w, region_h, 60, NULL);
            if (record_ctx == NULL)
            {
                fprintf(stderr, "error: could not open video output '%s'\n", record_output);
                free(record_pixels);
                record_pixels = NULL;
                record_region_w = record_region_h = 0;
                do_record_video = false;
            }
        }
    }

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
    close_recording();

    if (record_pixels != NULL)
    {
        free(record_pixels);
        record_pixels = NULL;
    }

    if (renderer != NULL)
    {
        od_terminate(renderer);
        renderer = NULL;
    }
    linear_free(&memory);
}

sapp_desc sokol_main(int argc, char* argv[])
{
    flag_bool_var(&do_screenshot, "screenshot", false, "capture the third frame to example_screenshot.png and quit");
    flag_bool_var(&do_record_video, "record-video", false, "record the Widgets window to a video until quit");
    flag_str_var(&record_output, "record-output", "example.avi", "video output file");

    if (!flag_parse(argc, argv))
    {
        flag_print_error(stderr);
        flag_print_options(stderr);
        exit(1);
    }

    if (do_screenshot && do_record_video)
    {
        fprintf(stderr, "-screenshot and -record-video are mutually exclusive\n");
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
        .window_title = "lean_ui example"
    };
}
