#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "bin2h.h"
#include "bc4_encoder.h"
#include "../src/font.h"
#include "../lib/onedraw.h"

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
#define ATLAS_SIZE_STANDARD  (256)
#define ATLAS_SIZE_HIGHDPI   (512)
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
bool build_font(Arena *arena, float font_height, uint32_t atlas_width, uint32_t atlas_height, const char* font_h, const char* glyph_h, const char* atlas_var, const char* glyph_var)
{
    if ((atlas_width%4) != 0 || (atlas_height%4) != 0)
    {
        fprintf(stderr, "error: the font atlas size (%ux%u) must be a multiple of 4\n", atlas_width, atlas_height);
        return false;
    }

    fprintf(stdout, "opening font '%s' : ", FONT_FILENAME);

    size_t font_size;
    uint8_t* font_data = read_file(FONT_FILENAME, &font_size, arena);

    if (font_data == NULL)
        return false; // read_file reported the error

    fprintf(stdout, "ok\n");

    stbtt_fontinfo font_info;
    if (!stbtt_InitFont(&font_info, font_data, 0))
    {
        fprintf(stderr, "error: '%s' is not a valid font file\n", FONT_FILENAME);
        return false;
    }

    fprintf(stdout, "baking %ux%u atlas : ", atlas_width, atlas_height);

    uint8_t* atlas_pixels = arena_alloc(arena, atlas_height * atlas_width);
    if (atlas_pixels == NULL)
    {
        fprintf(stderr, "error: out of memory while baking the font atlas\n");
        return false;
    }

    stbtt_bakedchar glyphs[FONT_NUM_CHARS];
    int result = stbtt_BakeFontBitmap(font_data, 0, font_height, atlas_pixels, atlas_width, atlas_height,
                                      FONT_CHAR_FIRST, FONT_NUM_CHARS, glyphs);

    if (result <= 0)
    {
        if (result == 0)
            fprintf(stderr, "error: the font atlas is too small to fit a single glyph at height %.0f, increase the atlas size\n", font_height);
        else
            fprintf(stderr, "error: the font atlas is too small: only %d of %d glyphs fit at height %.0f, increase the atlas size\n", -result, FONT_NUM_CHARS, font_height);
        return false;
    }

    fprintf(stdout, "ok\n");

    fprintf(stdout, "compressing atlas in BC4 : ");

    size_t bc4_image_size = (atlas_width/4) * (atlas_height/4) * 8;
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

    bool success = build_font(&arena, FONT_HEIGHT_STANDARD, ATLAS_SIZE_STANDARD, ATLAS_SIZE_STANDARD,
                              FONT_H, GLYPH_H, "default_font_atlas", "default_font");

    track_peak_memory(&arena, &peak_memory);
    arena_reset(&arena);

    success = success && build_font(&arena, FONT_HEIGHT_HIGHDPI, ATLAS_SIZE_HIGHDPI, ATLAS_SIZE_HIGHDPI,
                                    FONT_H_HIGHDPI, GLYPH_H_HIGHDPI, "default_font_atlas_highdpi", "default_font_highdpi");

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
