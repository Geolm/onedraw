#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "stb_image_write.h"

#define FLAG_IMPLEMENTATION
#include "flag.h"

#define SOKOL_METAL
#include "sokol_app.h"

#include "../lib/onedraw.h"
#include "scene.h"
#include "compare.h"

struct onedraw* renderer;
bool culling_debug = false;

// command line options, parsed in sokol_main
struct test_options
{
    bool autoexit;     // quit after 100 frames
    bool compare;      // exit non-zero if the screenshot differs from the reference
    bool update_refs;  // rewrite the matching reference image from the current capture
    bool cache;
    bool offscreen;    // render into the offscreen target instead of the window drawable
    bool validate;     // enable the metal api validation layer
};

struct test_options options = {false, false, false, false, false, false};

float gpu_time_ms = 0.f;
float binning_time_ms = 0.f;
float rasterization_time_ms = 0.f;
uint32_t num_frames = 0;
uint32_t frame_count = 0;

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
        .cache = options.cache,
        .allow_offscreen = options.offscreen,
        .atlas = 
        {
            .width = TEX_SIZE,
            .height = TEX_SIZE,
            .num_slices = 4
        }
    });

    if (renderer == NULL)
        return;

    scene_init(renderer);
}

// ---------------------------------------------------------------------------------------------------------------------------
void frame(void)
{
    if (renderer == NULL)
        return;

    // fully obscured window : macOS provides no drawable, sapp returns an invalid swapchain (the nil
    // drawable reads as 0x0 and would fail the onedraw size assert), so skip the whole frame until
    // visible (offscreen mode doesn't need a drawable, a hidden window does not stop the frames)
    sapp_swapchain sc = sapp_acquire_swapchain();
    if (sc.invalid && !options.offscreen)
        return;

    frame_count++;

    char string[256];

    od_begin_frame(renderer);
    scene_draw(renderer, frame_count);

    od_stats stats;
    od_get_stats(renderer, &stats);

    if (stats.frame_index > 60)
    {
        // average all gpu time to get close to the "real" value as the scene is static
        num_frames++;
        gpu_time_ms += stats.gpu_time_ms;
        binning_time_ms += stats.binning_time_ms;
        rasterization_time_ms += stats.rasterization_time_ms;

        const float label_size = CANVAS_HEIGHT / 30.f;
        const float line_height = od_text_line_height(renderer, od_font_bold, label_size);

        snprintf(string, 256, "GPU Memory usage : %zu kb", stats.gpu_memory_usage>>10);
        od_draw_text(renderer, 0, CANVAS_HEIGHT - line_height * 2.f, string, od_font_bold, label_size, miya_blue);

        snprintf(string, 256, "num commands : %u", stats.peak_num_draw_cmd);
        od_draw_text(renderer, (CANVAS_WIDTH - od_string_view_width(renderer, string, strlen(string), od_font_bold, label_size)) * .5f,
                    CANVAS_HEIGHT - line_height * 2.f, string, od_font_bold, label_size, miya_blue);

        snprintf(string, 256, "GPU Time : %2.2f ms", gpu_time_ms / (float)(num_frames));
        od_draw_text(renderer, CANVAS_WIDTH - od_text_width(renderer, string, od_font_bold, label_size),
                    CANVAS_HEIGHT - line_height * 2.f, string, od_font_bold, label_size, miya_blue);
    }

    uint8_t* pixels = NULL;
    if (stats.frame_index == 10)
    {
        pixels = malloc(sapp_width() * sapp_height() * 4);
        od_take_screenshot(renderer, pixels);
    }

    od_end_frame(renderer, options.offscreen ? NULL : (void*)sc.metal.current_drawable);

    if (pixels != NULL)
    {
        test_bgra_to_rgba(pixels, sapp_width(), sapp_height());
        stbi_write_png("screenshot.png", sapp_width(), sapp_height(), 4, pixels,  sapp_width() * sizeof(uint32_t));
        test_compare_reference(pixels, sapp_width(), sapp_height(), options.compare, options.update_refs);
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
    {
        if (num_frames > 0)
        {
            const float avg_gpu = gpu_time_ms / (float)num_frames;
            const float avg_binning = binning_time_ms / (float)num_frames;
            const float avg_rasterization = rasterization_time_ms / (float)num_frames;

            fprintf(stdout, "gpu time (last %u frames) : %.2f ms (binning %.2f ms, rasterization %.2f ms)\n",
                    num_frames, avg_gpu, avg_binning, avg_rasterization);

            if (options.validate)
                fprintf(stdout, "validation : clean\n");

            if (options.autoexit && (avg_binning > 0.f || avg_rasterization > 0.f))
            {
                // the fine timings are only measured when the counter sample buffer exists, in which case
                // each part must be positive and fit in the global buffer time (the tolerance covers the
                // tick->seconds conversion error); the sum is not checked, encoders can overlap
                if ((avg_gpu <= 0.f) || (avg_binning <= 0.f) || (avg_rasterization <= 0.f) ||
                    (avg_binning > avg_gpu * 1.05f + 0.00001f) || (avg_rasterization > avg_gpu * 1.05f + 0.00001f))
                {
                    fprintf(stderr, "gpu time check failed : expected 0 < binning, rasterization <= global gpu time\n");
                    exit(1);
                }
            }
        }

        od_stats stats;
        od_get_stats(renderer, &stats);
        fprintf(stdout, "gpu memory : %zu kb (peak num commands : %u)\n", stats.gpu_memory_usage >> 10, stats.peak_num_draw_cmd);

        od_terminate(renderer);
    }
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
    flag_bool_var(&options.offscreen, "offscreen", false, "render into the offscreen target instead of the window drawable");
    flag_bool_var(&options.validate, "validate", false, "enable the metal api validation layer");

    if (!flag_parse(argc, argv))
    {
        flag_print_error(stderr);
        flag_print_options(stderr);
        exit(1);
    }

    // must be set before the metal device is created in sapp_run; the layer aborts the process
    // on any validation error, so reaching cleanup() means the whole run was clean
    if (options.validate)
        setenv("MTL_DEBUG_LAYER", "1", 1);
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
