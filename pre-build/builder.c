#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "bin2h.h"
#include "bc4_encoder.h"
#include "../src/font.h"
#include "../lib/onedraw.h"

#define STB_RECT_PACK_IMPLEMENTATION
#include "stb_rect_pack.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include "shader_reader.h"

#define ARENA_NOSTDIO
#define ARENA_IMPLEMENTATION
#include "arena.h"



#define UNUSED_VARIABLE(a) (void)(a)

#define LIB_FOLDER "lib/"
#define FONT_FILENAME "fonts/Satoshi-Bold.otf"
#define FONT_HEIGHT_STANDARD (24.0f)
#define FONT_HEIGHT_HIGHDPI  (48.0f)
#define ATLAS_MAX_SIZE  (1024)
#define ATLAS_PADDING   (1)   // 1 px gutter, the packer stores it on the left and top of every glyph
#define FONT_H "lib/default_font_atlas.h"
#define GLYPH_H "lib/default_font.h"
#define FONT_H_HIGHDPI "lib/default_font_atlas_highdpi.h"
#define GLYPH_H_HIGHDPI "lib/default_font_highdpi.h"
#define FONT_CHAR_FIRST 33
#define FONT_CHAR_LAST 126
#define FONT_NUM_CHARS 95
#define SHADERS_FOLDER    "src/shaders/"
#define SOURCE_FOLDER   "src/"
#define BINNING_SHADER          "binning.metal"
#define RASTERIZER_SHADER       "rasterizer.metal"
#define CPU_GPU_COMMON_FILE     "common.h"


// ---------------------------------------------------------------------------------------------------------------------------
static bool file_exists(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (f == NULL)
        return false;

    fclose(f);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------------
// the builder relies on relative paths, make sure all the input files are present before doing any work
static bool verify_inputs(void)
{
    struct input
    {
        const char* path;
        const char* description;
    };

    const struct input inputs[] =
    {
        { FONT_FILENAME,                      "font file" },
        { SHADERS_FOLDER BINNING_SHADER,      "shader" },
        { SHADERS_FOLDER RASTERIZER_SHADER,   "shader" },
        { SHADERS_FOLDER CPU_GPU_COMMON_FILE, "header" },
    };

    bool ok = true;

    for (size_t i=0; i<sizeof(inputs)/sizeof(inputs[0]); ++i)
    {
        if (!file_exists(inputs[i].path))
        {
            fprintf(stderr, "error: could not find %s '%s'\n", inputs[i].description, inputs[i].path);
            ok = false;
        }
    }

    if (!ok)
        fprintf(stderr, "error: the builder must be run from the project root directory\n");

    return ok;
}

// ---------------------------------------------------------------------------------------------------------------------------
void* read_file(const char* filename, size_t* file_size, Arena* arena)
{
    *file_size = 0;

    FILE* f = fopen(filename, "rb");
    if (f == NULL)
    {
        fprintf(stderr, "error: could not open '%s' (%s)\n", filename, strerror(errno));
        return NULL;
    }

    fseek(f, 0L, SEEK_END);
    long size = ftell(f);
    fseek(f, 0L, SEEK_SET);

    if (size < 0)
    {
        fprintf(stderr, "error: could not determine the size of '%s' (%s)\n", filename, strerror(errno));
        fclose(f);
        return NULL;
    }

    *file_size = (size_t)size;

    if (*file_size == 0)
    {
        fprintf(stderr, "error: '%s' is empty\n", filename);
        fclose(f);
        return NULL;
    }

    void* buffer = arena_alloc(arena, *file_size);
    if (buffer == NULL)
    {
        fprintf(stderr, "error: out of memory while reading '%s'\n", filename);
        fclose(f);
        return NULL;
    }

    if (fread(buffer, *file_size, 1, f) != 1)
    {
        fprintf(stderr, "error: failed to read '%s' (%s)\n", filename, strerror(errno));
        fclose(f);
        return NULL;
    }

    fclose(f);
    return buffer;
}

// ---------------------------------------------------------------------------------------------------------------------------
static uint32_t round_up_4(uint32_t value)
{
    return (value + 3) & ~(uint32_t)3;
}

// ---------------------------------------------------------------------------------------------------------------------------
// can every glyph be packed into a (atlas_width x atlas_height) atlas ?
// this uses the exact same packing code path as the final bake, so a success here guarantees the bake succeeds
static bool can_pack(const stbtt_fontinfo* font_info, float font_height, uint32_t atlas_width, uint32_t atlas_height, stbrp_rect* rects)
{
    stbtt_pack_context pack_context;
    stbtt_pack_range range;

    memset(&pack_context, 0, sizeof(pack_context));
    if (!stbtt_PackBegin(&pack_context, NULL, (int)atlas_width, (int)atlas_height, 0, ATLAS_PADDING, NULL))
        return false; // out of memory

    memset(&range, 0, sizeof(range));
    range.font_size = font_height;
    range.first_unicode_codepoint_in_range = FONT_CHAR_FIRST;
    range.num_chars = FONT_NUM_CHARS;

    bool packed = stbtt_PackFontRangesGatherRects(&pack_context, font_info, &range, 1, rects) == FONT_NUM_CHARS;

    if (packed)
        packed = stbrp_pack_rects((stbrp_context*)pack_context.pack_info, rects, FONT_NUM_CHARS) == 1;

    stbtt_PackEnd(&pack_context);

    return packed;
}

// ---------------------------------------------------------------------------------------------------------------------------
// if the glyphs don't fit in a height, they don't fit in a smaller one either, so the search is monotonic
static uint32_t smallest_packing_height(const stbtt_fontinfo* font_info, float font_height, uint32_t atlas_width, uint32_t min_height, stbrp_rect* rects)
{
    // the packer's target is (width-1) x (height-1) because of the padding, so the atlas must be at least 1 bigger than the biggest rect
    uint32_t low = min_height / 4;
    uint32_t high = ATLAS_MAX_SIZE / 4;

    if (!can_pack(font_info, font_height, atlas_width, ATLAS_MAX_SIZE, rects))
        return 0; // doesn't fit even at the maximum height

    while (high > low)
    {
        uint32_t mid = low + (high - low) / 2;

        if (can_pack(font_info, font_height, atlas_width, mid*4, rects))
            high = mid;
        else
            low = mid + 1;
    }

    return high * 4;
}

// ---------------------------------------------------------------------------------------------------------------------------
// find the smallest atlas (width and height multiples of 4, at most ATLAS_MAX_SIZE) that fits every glyph
static bool find_atlas_size(const stbtt_fontinfo* font_info, float font_height, uint32_t* out_width, uint32_t* out_height, stbrp_rect* rects)
{
    // measure the glyph boxes to bound the search, each packed rect gets a +1 gutter on the left and top when packed
    float scale = stbtt_ScaleForPixelHeight(font_info, font_height);

    uint32_t max_rect_width = 0;
    uint32_t max_rect_height = 0;

    for (int i = 0; i < FONT_NUM_CHARS; ++i)
    {
        int x0, y0, x1, y1;

        stbtt_GetCodepointBitmapBox(font_info, FONT_CHAR_FIRST + i, scale, scale, &x0, &y0, &x1, &y1);

        if ((uint32_t)(x1 - x0 + ATLAS_PADDING) > max_rect_width)
            max_rect_width = (uint32_t)(x1 - x0 + ATLAS_PADDING);

        if ((uint32_t)(y1 - y0 + ATLAS_PADDING) > max_rect_height)
            max_rect_height = (uint32_t)(y1 - y0 + ATLAS_PADDING);
    }

    if (max_rect_width + 1 > ATLAS_MAX_SIZE)
    {
        fprintf(stderr, "error: the font is too wide (%u px) for an atlas of at most %u px, increase ATLAS_MAX_SIZE\n", max_rect_width, ATLAS_MAX_SIZE);
        return false;
    }

    struct size
    {
        uint16_t width;
        uint16_t height;
    };

    struct size sizes[ATLAS_MAX_SIZE/4 + 1];
    size_t num_sizes = 0;
    float min_area = 1e30f;

    for (uint32_t width = round_up_4(max_rect_width + 1); width <= ATLAS_MAX_SIZE; width += 4)
    {
        uint32_t height = smallest_packing_height(font_info, font_height, width, round_up_4(max_rect_height + 1), rects);

        if (height == 0)
            continue; // this width is too narrow

        float area = (float)width * (float)height;
        if (area < min_area)
            min_area = area;

        sizes[num_sizes++] = (struct size){(uint16_t)width, (uint16_t)height};
    }

    if (num_sizes == 0)
    {
        fprintf(stderr, "error: could not pack the font in an atlas of at most %ux%u, increase ATLAS_MAX_SIZE\n", ATLAS_MAX_SIZE, ATLAS_MAX_SIZE);
        return false;
    }

    // keep the smallest area, on a tie (or within 2%) prefer the squarer shape
    *out_width = 0;
    *out_height = 0;
    float best_aspect = 1000.f;

    for (size_t i = 0; i < num_sizes; ++i)
    {
        float area = (float)sizes[i].width * (float)sizes[i].height;

        if (area > min_area * 1.02f)
            continue;

        float aspect = (float)sizes[i].width > (float)sizes[i].height
            ? (float)sizes[i].width / (float)sizes[i].height
            : (float)sizes[i].height / (float)sizes[i].width;

        if (aspect < best_aspect)
        {
            best_aspect = aspect;
            *out_width = sizes[i].width;
            *out_height = sizes[i].height;
        }
    }

    return true;
}

// ---------------------------------------------------------------------------------------------------------------------------
bool build_font(Arena *arena, float font_height, const char* font_h, const char* glyph_h, const char* atlas_var, const char* glyph_var)
{
    fprintf(stdout, "opening font '%s' : ", FONT_FILENAME);

    size_t font_size;
    uint8_t* font_data = read_file(FONT_FILENAME, &font_size, arena);

    if (font_data == NULL)
        return false; // read_file reported the error

    fprintf(stdout, "ok\n");

    stbtt_fontinfo font_info = {0};
    if (!stbtt_InitFont(&font_info, font_data, 0))
    {
        fprintf(stderr, "error: '%s' is not a valid font file\n", FONT_FILENAME);
        return false;
    }

    fprintf(stdout, "sizing atlas : ");

    stbrp_rect rects[FONT_NUM_CHARS];
    uint32_t atlas_width, atlas_height;

    if (!find_atlas_size(&font_info, font_height, &atlas_width, &atlas_height, rects))
        return false; // find_atlas_size reported the error

    if ((atlas_width%4) != 0 || (atlas_height%4) != 0)
    {
        fprintf(stderr, "error: the font atlas size (%ux%u) must be a multiple of 4\n", atlas_width, atlas_height);
        return false;
    }

    size_t bc4_image_size = (atlas_width/4) * (atlas_height/4) * 8;

    fprintf(stdout, "%ux%u (%zu bytes of bc4 data)\n", atlas_width, atlas_height, bc4_image_size);

    fprintf(stdout, "baking %ux%u atlas : ", atlas_width, atlas_height);

    uint8_t* atlas_pixels = arena_alloc(arena, atlas_height * atlas_width);
    if (atlas_pixels == NULL)
    {
        fprintf(stderr, "error: out of memory while baking the font atlas\n");
        return false;
    }

    stbtt_packedchar glyphs[FONT_NUM_CHARS];
    memset(glyphs, 0, sizeof(glyphs));

    stbtt_pack_context pack_context;
    stbtt_pack_range range;

    memset(&pack_context, 0, sizeof(pack_context));
    if (!stbtt_PackBegin(&pack_context, atlas_pixels, (int)atlas_width, (int)atlas_height, 0, ATLAS_PADDING, NULL))
    {
        fprintf(stderr, "error: out of memory while baking the font atlas\n");
        return false;
    }

    memset(&range, 0, sizeof(range));
    range.font_size = font_height;
    range.first_unicode_codepoint_in_range = FONT_CHAR_FIRST;
    range.num_chars = FONT_NUM_CHARS;
    range.chardata_for_range = glyphs;

    bool packed = stbtt_PackFontRange(&pack_context, font_data, 0, font_height, FONT_CHAR_FIRST, FONT_NUM_CHARS, glyphs) == 1;
    stbtt_PackEnd(&pack_context);

    if (!packed)
    {
        fprintf(stderr, "error: the font atlas (%ux%u) is too small to fit every glyph at height %.0f, increase ATLAS_MAX_SIZE\n",
                atlas_width, atlas_height, font_height);
        return false;
    }

    fprintf(stdout, "ok\n");

    fprintf(stdout, "compressing atlas in BC4 : ");

    uint8_t* compressed_atlas = arena_alloc(arena, bc4_image_size);

    if (compressed_atlas == NULL)
    {
        fprintf(stderr, "error: out of memory while compressing the font atlas\n");
        return false;
    }

    bc4_encode(atlas_pixels, compressed_atlas, atlas_width, atlas_height);

    fprintf(stdout, "ok\n");

    fprintf(stdout, "writting %s : ", font_h);

    if (!bin2h(font_h, atlas_var, compressed_atlas, bc4_image_size))
        return false; // bin2h reported the error

    fprintf(stdout, "ok\n");

    fprintf(stdout, "filling glyphs structure : ");

    struct alphabet* font = arena_alloc(arena, sizeof(struct alphabet));
    if (font == NULL)
    {
        fprintf(stderr, "error: out of memory while building the glyphs structure\n");
        return false;
    }

    memset(font, 0, sizeof(struct alphabet));
    font->first_glyph = FONT_CHAR_FIRST;
    font->num_glyphs = FONT_NUM_CHARS;
    font->texture_width = atlas_width;
    font->texture_height = atlas_height;
    font->font_height = font_height;

    for(uint32_t i=0; i<FONT_NUM_CHARS; ++i)
    {
        font->glyphs[i] = (struct glyph)
        {
            .x0 = glyphs[i].x0,
            .y0 = glyphs[i].y0,
            .x1 = glyphs[i].x1,
            .y1 = glyphs[i].y1,
            .bearing_x = glyphs[i].xoff,
            .bearing_y = glyphs[i].yoff,
            .advance_x = glyphs[i].xadvance
        };
    }

    fprintf(stdout, "ok\n");

    fprintf(stdout, "writting %s : ", glyph_h);

    if (!bin2h(glyph_h, glyph_var, font, sizeof(struct alphabet)))
        return false; // bin2h reported the error

    fprintf(stdout, "ok\n");
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------------
bool prepare_shaders(Arena *arena)
{
    fprintf(stdout, "\npreparing shaders\n");

    fprintf(stdout, "opening %s : ", BINNING_SHADER);
    char* shader = read_shader_include(arena, SHADERS_FOLDER, BINNING_SHADER);

    if (shader == NULL)
        return false; // read_shader_include reported the error

    if (strlen(shader) == 0)
    {
        fprintf(stderr, "error: shader '%s%s' is empty\n", SHADERS_FOLDER, BINNING_SHADER);
        return false;
    }

    fprintf(stdout, "ok\nwritting %s : ", LIB_FOLDER"binning.h");
    if (!string2h(LIB_FOLDER"binning.h", "binning_shader", shader, strlen(shader)))
        return false; // string2h reported the error

    fprintf(stdout, "ok\n");

    fprintf(stdout, "opening %s : ", RASTERIZER_SHADER);
    shader = read_shader_include(arena, SHADERS_FOLDER, RASTERIZER_SHADER);

    if (shader == NULL)
        return false; // read_shader_include reported the error

    if (strlen(shader) == 0)
    {
        fprintf(stderr, "error: shader '%s%s' is empty\n", SHADERS_FOLDER, RASTERIZER_SHADER);
        return false;
    }

    fprintf(stdout, "ok\nwritting %s : ", LIB_FOLDER"rasterization.h");
    if (!string2h(LIB_FOLDER"rasterization.h", "rasterization_shader", shader, strlen(shader)))
        return false; // string2h reported the error

    fprintf(stdout, "ok\n");
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------------
bool copy_files(void)
{
    fprintf(stdout, "copying %s : ", SHADERS_FOLDER"common.h");

    if (!copy_file(SHADERS_FOLDER"common.h", LIB_FOLDER"common.h"))
        return false; // copy_file reported the error

    fprintf(stdout, "ok\n");

    return true;
}

// ---------------------------------------------------------------------------------------------------------------------------
static void track_peak_memory(Arena* arena, size_t* peak)
{
    size_t bytes_allocated, bytes_used;
    arena_stats(arena, &bytes_allocated, &bytes_used);

    if (bytes_allocated > *peak)
        *peak = bytes_allocated;
}

// ---------------------------------------------------------------------------------------------------------------------------
int main(int argc, const char * argv[]) 
{
    UNUSED_VARIABLE(argc);
    UNUSED_VARIABLE(argv);

    fprintf(stdout, "onedraw %u.%u library builder\n\n", ONEDRAW_MAJOR_VERSION, ONEDRAW_MINOR_VERSION);

    if (!verify_inputs())
        return 1;

    Arena arena = {0};
    size_t peak_memory = 0;

    bool success = build_font(&arena, FONT_HEIGHT_STANDARD, FONT_H, GLYPH_H, "default_font_atlas", "default_font");

    track_peak_memory(&arena, &peak_memory);
    arena_reset(&arena);

    success = success && build_font(&arena, FONT_HEIGHT_HIGHDPI, FONT_H_HIGHDPI, GLYPH_H_HIGHDPI, "default_font_atlas_highdpi", "default_font_highdpi");

    track_peak_memory(&arena, &peak_memory);
    arena_reset(&arena);

    success = success && prepare_shaders(&arena);

    track_peak_memory(&arena, &peak_memory);
    arena_reset(&arena);

    success = success && copy_files();

    fprintf(stdout, "\npeak memory usage %zukb\n\n", peak_memory>>10);
    arena_free(&arena);

    if (success)
        fprintf(stdout, "build succeeded\n");
    else
        fprintf(stderr, "build failed\n");

    return success ? 0 : 1;
}
