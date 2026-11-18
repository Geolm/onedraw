#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <dirent.h>
#include <unistd.h>

#include "stb_image_write.h"

#define FLAG_IMPLEMENTATION
#include "flag.h"

#define ARENA_IMPLEMENTATION
#include "arena.h"

#define SOKOL_METAL
#include "sokol_app.h"

#include "../lib/onedraw.h"
#include "../extra/svg.h"
#include "compare.h"

#define VIEW_WIDTH (1280)
#define VIEW_HEIGHT (720)
#define NUM_FRAMES (100)
#define CAPTURE_FRAME (10)
#define GRID_COLS (3)
#define GRID_MARGIN (20.f)
#define CLEAR_COLOR (0xff1e1e28)
#define TEX_SIZE (256)
#define MAX_FILES (64)

// command line options, parsed in sokol_main
struct test_options
{
    bool offscreen;   // render into the offscreen target at a fixed 1280x720 (the only mode that captures)
    bool compare;     // exit non-zero if the screenshot differs from the reference
    bool update_refs; // rewrite the reference image from the current capture
    bool validate;    // enable the metal api validation layer
};

struct test_options options = {false, false, false, false};

struct onedraw* renderer;
Arena arena;

struct svg_file
{
    char name[256];
    svg_document* doc;
    svg_draw_list* list;
    float x, y;       // cell origin (fit and centered)
    float scale;
    float doc_w, doc_h;
};

static struct svg_file files[MAX_FILES];
static uint32_t num_files;
static uint32_t viewport_w = VIEW_WIDTH;
static uint32_t viewport_h = VIEW_HEIGHT;

// --------------------------------------------------------------------------------------------------------------------------------
static void custom_log(const char* string)
{
    fprintf(stdout, "%s\n", string);
}

// --------------------------------------------------------------------------------------------------------------------------------
static void* svg_alloc_cb(void* user_data, size_t size, size_t alignment)
{
    (void) user_data;
    (void) alignment;
    return arena_alloc(&arena, size);
}

static svg_allocator svg_arena(void)
{
    svg_allocator a = {svg_alloc_cb, NULL, NULL};
    return a;
}

// --------------------------------------------------------------------------------------------------------------------------------
// The reference is committed at the repo root; the binary runs from the build dir (CI) or
// the repo root (dev), so try both
static const char* reference_path(void)
{
    const char* candidates[2] = {"tests/references/svg.png", "../tests/references/svg.png"};
    for (uint32_t i = 0; i < 2; ++i)
    {
        FILE* f = fopen(candidates[i], "rb");
        if (f != NULL)
        {
            fclose(f);
            return candidates[i];
        }
    }
    for (uint32_t i = 0; i < 2; ++i)
    {
        const char* dir = (i == 0) ? "tests/references" : "../tests/references";
        if (access(dir, W_OK) == 0)
            return candidates[i];
    }
    return candidates[0];
}

// --------------------------------------------------------------------------------------------------------------------------------
static bool load_one(const char* path, const char* name)
{
    if (num_files >= MAX_FILES)
    {
        fprintf(stderr, "svg test : more than %d files\n", MAX_FILES);
        return false;
    }

    FILE* f = fopen(path, "rb");
    if (f == NULL)
    {
        fprintf(stderr, "svg test : cannot open %s\n", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* text = (char*)arena_alloc(&arena, (size_t)size + 1);
    if (fread(text, 1, (size_t)size, f) != (size_t)size)
    {
        fprintf(stderr, "svg test : cannot read %s\n", path);
        fclose(f);
        return false;
    }
    fclose(f);
    text[size] = '\0';

    struct svg_file* file = &files[num_files];
    snprintf(file->name, sizeof(file->name), "%s", name);
    file->doc = svg_parse(sv_from_parts(text, (size_t)size), svg_arena());
    if (file->doc == NULL)
    {
        fprintf(stderr, "svg test : failed to parse %s\n", path);
        return false;
    }
    svg_document_size(file->doc, &file->doc_w, &file->doc_h);
    uint32_t flags = svg_document_flags(file->doc);
    if (flags != 0)
        fprintf(stdout, "svg test : %s uses unsupported features (flags 0x%x)\n", name, flags);
    ++num_files;
    return true;
}

// the fixed set : every tests/svg/*.svg in name order ; a missing directory fails the test
static bool load_fixed_set(void)
{
    static const char* dirs[] = {"tests/svg", "../tests/svg"};
    const char* dir = NULL;
    for (uint32_t i = 0; i < 2; ++i)
    {
        DIR* d = opendir(dirs[i]);
        if (d != NULL)
        {
            dir = dirs[i];
            closedir(d);
            break;
        }
    }
    if (dir == NULL)
    {
        fprintf(stderr, "svg test : tests/svg/ not found\n");
        return false;
    }

    char names[MAX_FILES][256];
    uint32_t num_names = 0;
    DIR* d = opendir(dir);
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL && num_names < MAX_FILES)
    {
        const char* name = ent->d_name;
        size_t len = strlen(name);
        if (len > 4 && strcmp(name + len - 4, ".svg") == 0)
            snprintf(names[num_names++], sizeof(names[0]), "%s", name);
    }
    closedir(d);

    if (num_names == 0)
    {
        fprintf(stderr, "svg test : no .svg files in %s/\n", dir);
        return false;
    }
    qsort(names, num_names, sizeof(names[0]), (int(*) (const void*, const void*))strcmp);

    for (uint32_t i = 0; i < num_names; ++i)
    {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        if (!load_one(path, names[i]))
            return false;
    }
    return true;
}

// --------------------------------------------------------------------------------------------------------------------------------
// the fixed grid layout : files in name order, each fitted (and centered) in its cell
static void layout(uint32_t w, uint32_t h)
{
    uint32_t rows = (num_files + GRID_COLS - 1) / GRID_COLS;
    float cell_w = ((float)w - GRID_MARGIN * (GRID_COLS + 1)) / GRID_COLS;
    float cell_h = ((float)h - GRID_MARGIN * (rows + 1)) / rows;
    for (uint32_t i = 0; i < num_files; ++i)
    {
        float scale = cell_w / files[i].doc_w;
        float s2 = cell_h / files[i].doc_h;
        if (s2 < scale)
            scale = s2;
        float cw = files[i].doc_w * scale;
        float ch = files[i].doc_h * scale;
        files[i].scale = scale;
        files[i].x = GRID_MARGIN + (i % GRID_COLS) * (cell_w + GRID_MARGIN) + (cell_w - cw) * .5f;
        files[i].y = GRID_MARGIN + (i / GRID_COLS) * (cell_h + GRID_MARGIN) + (cell_h - ch) * .5f;
    }
}

static void prepare_lists(uint32_t w, uint32_t h)
{
    layout(w, h);
    for (uint32_t i = 0; i < num_files; ++i)
    {
        if (files[i].list)
            svg_free_draw_list(files[i].list, svg_arena());
        files[i].list = svg_prepare(files[i].doc, files[i].scale, 0.25f, svg_arena());
        if (files[i].list == NULL)
        {
            fprintf(stderr, "svg test : failed to prepare %s\n", files[i].name);
            exit(1);
        }
    }
}

// --------------------------------------------------------------------------------------------------------------------------------
static void init(void)
{
    sapp_environment env = sapp_get_environment();

    if (flag_rest_argc() > 0)
    {
        const char** argv = (const char**)flag_rest_argv();
        for (int i = 0; i < flag_rest_argc(); ++i)
        {
            const char* path = argv[i];
            const char* name = path;
            for (const char* p = path; *p != '\0'; ++p)
                if (*p == '/')
                    name = p + 1;
            if (!load_one(path, name))
                exit(1);
        }
    }
    else if (!load_fixed_set())
        exit(1);

    if (options.offscreen)
    {
        viewport_w = VIEW_WIDTH;
        viewport_h = VIEW_HEIGHT;
    }
    else
    {
        viewport_w = (uint32_t)sapp_width();
        viewport_h = (uint32_t)sapp_height();
    }

    renderer = od_init(&(onedraw_def)
    {
        .metal_device = (void*)env.metal.device,
        .preallocated_buffer = malloc(od_min_memory_size()),
        .viewport_width = viewport_w,
        .viewport_height = viewport_h,
        .log_func = custom_log,
        .srgb_backbuffer = false,
        .allow_screenshot = true,
        .allow_offscreen = options.offscreen,
        .atlas =
        {
            .width = TEX_SIZE,
            .height = TEX_SIZE,
            .num_slices = 4
        }
    });

    if (renderer == NULL)
    {
        if (options.offscreen)
            exit(1);
        return;
    }

    od_set_clear_color(renderer, CLEAR_COLOR);
    prepare_lists(viewport_w, viewport_h);
}

// --------------------------------------------------------------------------------------------------------------------------------
static void frame(void)
{
    if (renderer == NULL)
    {
        if (options.offscreen)
            exit(1);
        return;
    }

    // fully obscured window : macOS provides no drawable, sapp returns an invalid swapchain (the
    // nil drawable reads as 0x0 and would fail the onedraw size assert), so skip the whole frame
    // until visible (offscreen mode doesn't need a drawable, a hidden window does not stop the frames)
    sapp_swapchain sc = sapp_acquire_swapchain();
    if (sc.invalid && !options.offscreen)
        return;

    uint32_t w = viewport_w;
    uint32_t h = viewport_h;

    od_stats stats;
    od_get_stats(renderer, &stats);

    od_begin_frame(renderer);
    for (uint32_t i = 0; i < num_files; ++i)
    {
        svg_draw(renderer, files[i].list, files[i].x, files[i].y, w, h);
    }

    uint8_t* pixels = NULL;
    if (options.offscreen && stats.frame_index == CAPTURE_FRAME)
    {
        pixels = (uint8_t*)malloc((size_t)w * h * 4);
        od_take_screenshot(renderer, pixels);
    }

    od_end_frame(renderer, options.offscreen ? NULL : (void*)sc.metal.current_drawable);

    if (pixels != NULL)
    {
        test_bgra_to_rgba(pixels, w, h);
        stbi_write_png("screenshot_svg.png", (int)w, (int)h, 4, pixels, w * sizeof(uint32_t));
        const char* ref = reference_path();
        test_compare_reference_named(pixels, w, h, ref, options.compare, options.update_refs);
        free(pixels);
    }

    // offscreen mode is a batch run : stop once the frame budget is used up
    if (options.offscreen && stats.frame_index == NUM_FRAMES)
        sapp_quit();
}

// --------------------------------------------------------------------------------------------------------------------------------
static void event(const sapp_event* event)
{
    if (renderer == NULL)
        return;

    switch (event->type)
    {
    case SAPP_EVENTTYPE_RESIZED:
        if (!options.offscreen)
        {
            viewport_w = (uint32_t)event->framebuffer_width;
            viewport_h = (uint32_t)event->framebuffer_height;
            od_resize(renderer, event->framebuffer_width, event->framebuffer_height);
            prepare_lists(viewport_w, viewport_h);
        }
        break;
    case SAPP_EVENTTYPE_KEY_DOWN:
        if (event->key_code == SAPP_KEYCODE_Q && event->modifiers == SAPP_MODIFIER_SUPER)
            sapp_quit();
        break;
    default:
        break;
    }
}

// --------------------------------------------------------------------------------------------------------------------------------
static void cleanup(void)
{
    if (renderer != NULL)
    {
        if (options.validate)
            fprintf(stdout, "validation : clean\n");

        uint32_t num_commands = 0, num_data_floats = 0;
        for (uint32_t i = 0; i < num_files; ++i)
        {
            uint32_t c, d;
            svg_report(files[i].list, &c, &d);
            fprintf(stdout, "svg [%s] : %u commands, %u draw data floats\n", files[i].name, c, d);
            num_commands += c;
            num_data_floats += d;
        }

        od_stats stats;
        od_get_stats(renderer, &stats);
        fprintf(stdout, "svg : %u files, %u commands, %u draw data floats\n", num_files, num_commands, num_data_floats);
        fprintf(stdout, "gpu memory : %zu kb (peak num commands : %u)\n", stats.gpu_memory_usage >> 10,
                stats.peak_num_draw_cmd);

        od_terminate(renderer);
    }
    free(renderer);
    renderer = NULL;
}

// --------------------------------------------------------------------------------------------------------------------------------
sapp_desc sokol_main(int argc, char* argv[])
{
    // flag.h matches single-dash names; normalize "--foo" to "-foo" so both forms work
    for (int i = 1; i < argc; ++i)
    {
        if (argv[i][0] == '-' && argv[i][1] == '-' && argv[i][2] != '\0')
            memmove(argv[i] + 1, argv[i] + 2, strlen(argv[i]) - 1);
    }

    flag_bool_var(&options.offscreen, "offscreen", false,
                  "render into the offscreen target at a fixed 1280x720 (the only mode that captures)");
    flag_bool_var(&options.compare, "compare", false, "exit non-zero if the screenshot differs from the reference");
    flag_bool_var(&options.update_refs, "update-refs", false, "rewrite the reference image from the current capture");
    flag_bool_var(&options.validate, "validate", false, "enable the metal api validation layer");

    if (!flag_parse(argc, argv))
    {
        flag_print_error(stderr);
        flag_print_options(stderr);
        exit(1);
    }

    if ((options.compare || options.update_refs) && !options.offscreen)
    {
        fprintf(stderr, "--compare and --update-refs require --offscreen\n");
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
        .window_title = "onedraw svg"
    };
}
