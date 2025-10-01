#include <stdio.h>
#include <string.h>
#include "bin2h.h"
#include "bc4_encoder.h"
#include "../lib/renderer.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#define ARENA_NOSTDIO
#define ARENA_IMPLEMENTATION
#include "arena.h"

#define UNUSED_VARIABLE(a) (void)(a)

#define FONT_FILENAME "../fonts/CommitMono-400-Regular.otf"
#define FONT_H "../lib/default_font_atlas.h"
#define GLYPH_H "../lib/default_font.h"
#define FONT_CHAR_FIRST 33
#define FONT_CHAR_LAST 126
#define FONT_NUM_CHARS 95

#define SDF2D_MAJOR_VERSION (0)
#define SDF2D_MINOR_VERSION (1)

// ---------------------------------------------------------------------------------------------------------------------------
void* read_file(const char* filename, size_t* file_size, Arena* arena)
{
    FILE* f = fopen(filename, "rb");
    if (f != NULL)
    {
        fseek(f, 0L, SEEK_END);
        *file_size = ftell(f);
        fseek(f, 0L, SEEK_SET);

        void* buffer = arena_alloc(arena, (*file_size));
        if (fread(buffer, *file_size, 1, f) == 1)
            return buffer;
    }
    return NULL;
}

// ---------------------------------------------------------------------------------------------------------------------------
bool build_font(Arena *arena, float font_height, uint32_t atlas_width, uint32_t atlas_height)
{
    if ((atlas_width%4) != 0 || (atlas_height%4) != 0)
    {
        fprintf(stdout, "font atlas width and height must be a multiple of 4\n");
        return false;
    }

    fprintf(stdout, "opening font \'%s\' : ", FONT_FILENAME);

    size_t font_size;
    uint8_t* font_data = read_file(FONT_FILENAME, &font_size, arena);

    if (font_data == NULL)
        return false;

    fprintf(stdout, "ok\nbaking %dx%d atlas : ", atlas_width, atlas_height);

    uint8_t* atlas_pixels = arena_alloc(arena, atlas_height * atlas_width);
    if (atlas_pixels == NULL)
        return false;

    stbtt_bakedchar glyphs[FONT_NUM_CHARS];
    int result = stbtt_BakeFontBitmap(font_data, 0, font_height, atlas_pixels, atlas_width, atlas_height,
                                      FONT_CHAR_FIRST, FONT_NUM_CHARS, glyphs);

    if (result == 0)
        return false;
    else if (result < 0)
    {
        fprintf(stdout, "warning only %d chars could fit in the atlas\n", -result);
    }
    else fprintf(stdout, "ok\n");

    fprintf(stdout, "compressing atlas in BC4 : ");

    size_t bc4_image_size = (atlas_width/4) * (atlas_height/4) * 8;
    uint8_t* compressed_atlas = arena_alloc(arena, bc4_image_size);

    if (compressed_atlas == NULL)
        return false;

    bc4_encode(atlas_pixels, compressed_atlas, atlas_width, atlas_height);

    fprintf(stdout, "ok\nwritting %s : ", FONT_H);

    if (!bin2h(FONT_H, "default_font_atlas", compressed_atlas, bc4_image_size))
        return false;

    fprintf(stdout, "ok\nfilling glyphs structure : ");

    font_t* font = arena_alloc(arena, sizeof(font_t));
    if (font == NULL)
        return false;

    memset(font, 0, sizeof(font_t));
    font->first_glyph = FONT_CHAR_FIRST;
    font->num_glyphs = FONT_NUM_CHARS;
    font->texture_width = atlas_width;
    font->texture_height = atlas_height;
    font->font_height = font_height;

    for(uint32_t i=0; i<FONT_NUM_CHARS; ++i)
    {
        font->glyphs[i] = (font_glyph_t)
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

    fprintf(stdout, "ok\nwritting %s : ", GLYPH_H);

    if (!bin2h(GLYPH_H, "default_font", font, sizeof(font_t)))
        return false;

    fprintf(stdout, "ok\n");
    return true;
}


// ---------------------------------------------------------------------------------------------------------------------------
int main(int argc, const char * argv[]) 
{
    UNUSED_VARIABLE(argc);
    UNUSED_VARIABLE(argv);

    Arena arena = {0};

    fprintf(stdout, "sdf2d %u.%u library builder\n\n", SDF2D_MAJOR_VERSION, SDF2D_MINOR_VERSION);

    bool success = build_font(&arena, 32.f, 256, 256);
    if (!success)
        fprintf(stdout, "failed\n");

    arena_reset(&arena);

    size_t bytes_allocated, bytes_used;
    arena_stats(&arena, &bytes_allocated, &bytes_used);
    fprintf(stdout, "\npeak memory usage %zukb\n", bytes_allocated>>10);
    arena_free(&arena);

    return success ? 0 : -1;
}
