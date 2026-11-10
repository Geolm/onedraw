#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "stb_image_write.h"

#define FLAG_IMPLEMENTATION
#include "flag.h"

#include "../lib/onedraw.h"
#include "scene.h"
#include "compare.h"

struct onedraw* renderer;

#define VIEW_WIDTH (1280)
#define VIEW_HEIGHT (720)
#define NUM_FRAMES (100)
#define CAPTURE_FRAME (10)

struct test_options
{
    bool compare;      // exit non-zero if the screenshot differs from the reference
    bool update_refs;  // rewrite the matching reference image from the current capture
    bool cache;
    bool validate;     // enable the metal api validation layer
};

struct test_options options = {false, false, false, false};

// ---------------------------------------------------------------------------------------------------------------------------
static void custom_log(const char* string)
{
    fprintf(stdout, "%s\n", string);
}

// ---------------------------------------------------------------------------------------------------------------------------
int main(int argc, char* argv[])
{
    // flag.h matches single-dash names; normalize "--foo" to "-foo" so both forms work
    for (int i = 1; i < argc; ++i)
    {
        if (argv[i][0] == '-' && argv[i][1] == '-' && argv[i][2] != '\0')
            memmove(argv[i] + 1, argv[i] + 2, strlen(argv[i]) - 1);
    }

    flag_bool_var(&options.compare, "compare", false, "exit non-zero if the screenshot differs from the reference");
    flag_bool_var(&options.update_refs, "update-refs", false, "rewrite the matching reference image from the current capture");
    flag_bool_var(&options.cache, "cache", false, "enable the tile cache");
    flag_bool_var(&options.validate, "validate", false, "enable the metal api validation layer");

    if (!flag_parse(argc, argv))
    {
        flag_print_error(stderr);
        flag_print_options(stderr);
        exit(1);
    }

    // must be set before the metal device is created; the layer aborts the process on any
    // validation error, so reaching the end means the whole run was clean
    if (options.validate)
        setenv("MTL_DEBUG_LAYER", "1", 1);

    if (!od_is_supported())
    {
        custom_log("no m1/a14 (or later) gpu, headless test skipped");
        return 0;
    }

    void* preallocated_buffer = malloc(od_min_memory_size());
    renderer = od_init( &(onedraw_def)
    {
        .preallocated_buffer = preallocated_buffer,
        .viewport_width = VIEW_WIDTH,
        .viewport_height = VIEW_HEIGHT,
        .log_func = custom_log,
        .srgb_backbuffer = false,
        .allow_screenshot = true,
        .cache = options.cache,
        .allow_offscreen = true,
        .atlas =
        {
            .width = TEX_SIZE,
            .height = TEX_SIZE,
            .num_slices = 4
        }
    });

    if (renderer == NULL)
    {
        free(preallocated_buffer);
        return 1;
    }

    scene_init(renderer);

    uint8_t* pixels = NULL;
    for (uint32_t frame = 0; frame < NUM_FRAMES; ++frame)
    {
        od_begin_frame(renderer);
        scene_draw(renderer, frame + 1);

        if (frame + 1 == CAPTURE_FRAME)
        {
            pixels = malloc(VIEW_WIDTH * VIEW_HEIGHT * 4);
            od_take_screenshot(renderer, pixels);
        }

        od_end_frame(renderer, NULL);

        if (pixels != NULL)
        {
            test_bgra_to_rgba(pixels, VIEW_WIDTH, VIEW_HEIGHT);
            stbi_write_png("screenshot_headless.png", VIEW_WIDTH, VIEW_HEIGHT, 4, pixels, VIEW_WIDTH * sizeof(uint32_t));
            test_compare_reference(pixels, VIEW_WIDTH, VIEW_HEIGHT, options.compare, options.update_refs);
            free(pixels);
            pixels = NULL;
        }
    }

    if (options.validate)
        fprintf(stdout, "validation : clean\n");

    od_stats stats;
    od_get_stats(renderer, &stats);
    fprintf(stdout, "gpu memory : %zu kb (peak num commands : %u)\n", stats.gpu_memory_usage >> 10, stats.peak_num_draw_cmd);

    od_terminate(renderer);
    free(renderer);
    renderer = NULL;

    return 0;
}
