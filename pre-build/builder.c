#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "bin2h.h"
#include "bc4_encoder.h"
#include "bc_packed.h"
#include "../src/shaders/common.h"
#include "../lib/onedraw.h"

#define STB_RECT_PACK_IMPLEMENTATION
#include "stb_rect_pack.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

// arena.h : the backend must be selected before its first inclusion, and the implementation
// pass (ARENA_IMPLEMENTATION, which sits outside the include guard) must run exactly once
#define ARENA_BACKEND ARENA_BACKEND_LINUX_MMAP

#define ARENA_NOSTDIO
#define ARENA_IMPLEMENTATION
#include "arena.h"



#define LIB_FOLDER "lib/"
#define FONT_FILENAME_REGULAR   "fonts/Satoshi-Regular.otf"
#define FONT_FILENAME_BOLD      "fonts/Satoshi-Bold.otf"
#define FONT_FILENAME_ITALIC    "fonts/Satoshi-Italic.otf"
#define FONT_FILENAME_MONO      "fonts/JetBrainsMono-Regular.ttf"
#define ATLAS_MAX_SIZE  (2048)
#define ATLAS_PADDING   (1)   // 1 px gutter, the packer stores it on the left and top of every glyph
#define FONT_H "lib/default_font_atlas.h"
#define GLYPH_H "lib/default_font.h"
#define SHADERS_H "lib/shaders.h"
#define SHADERS_FOLDER    "src/shaders/"
#define CPU_GPU_COMMON_FILE     "common.h"

// the sizes are physical pixels, shared by every font, and must match OD_NUM_FONT_SIZES
static const float FONT_SIZES[OD_NUM_FONT_SIZES] = {12.0f, 16.0f, 20.0f, 24.0f, 32.0f, 40.0f, 48.0f, 56.0f, 64.0f};
static const char* const FONT_FILES[OD_NUM_FONTS] = {FONT_FILENAME_REGULAR, FONT_FILENAME_BOLD, FONT_FILENAME_ITALIC, FONT_FILENAME_MONO};

static uint32_t CODEPOINTS[MAX_GLYPHS];
static int NUM_CODEPOINTS;

// the baked character set, identical for every font, sorted (the glyph index is the position in this list)
static bool build_codepoints(void)
{
    int i = 0;

    for (int c = 0x20; c <= 0x7E; ++c)
        CODEPOINTS[i++] = (uint32_t)c;

    for (int c = 0xA1; c <= 0xFF; ++c)
    {
        if (c == 0xAA || c == 0xBA || c == 0xAD)
            continue;
        CODEPOINTS[i++] = (uint32_t)c;
    }

    const uint32_t extras[] =
    {
        0x0152, 0x0153, 0x0178, 0x03A9, 0x03C0, 0x2013, 0x2014, 0x2018, 0x2019, 0x201A, 0x201C, 0x201D,
        0x201E, 0x2020, 0x2021, 0x2022, 0x2026, 0x2030, 0x2032, 0x2039, 0x203A, 0x20AC, 0x2122, 0x2190,
        0x2191, 0x2192, 0x2193, 0x2194, 0x2195, 0x2202, 0x2206, 0x220F, 0x2211, 0x2212, 0x221A, 0x221E,
        0x222B, 0x2248, 0x2260, 0x2264, 0x2265, 0x25A0, 0x25A1, 0x25B2, 0x25CB, 0x25CF, 0x2713
    };

    for (int e = 0; e < (int)(sizeof(extras) / sizeof(extras[0])); ++e)
        CODEPOINTS[i++] = extras[e];

    // the runtime binary search requires a strictly ascending list : a future out-of-order addition
    // must fail the build instead of silently baking a glyph that renders as a space
    for (int c = 1; c < i; ++c)
    {
        if (CODEPOINTS[c - 1] >= CODEPOINTS[c])
        {
            fprintf(stderr, "error: the code point set must be declared in ascending order\n");
            return false;
        }
    }

    // the runtime maps printable ascii by direct offset (cp - 0x20), so the ascii block must stay at the start
    for (int c = 0x20; c <= 0x7E; ++c)
    {
        if (CODEPOINTS[c - 0x20] != (uint32_t)c)
        {
            fprintf(stderr, "error: the code point set must not contain characters below 0x20 (the runtime maps ascii by direct offset)\n");
            return false;
        }
    }

    NUM_CODEPOINTS = i;

    return (i == 234 && i <= MAX_GLYPHS);
}

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
    bool ok = true;

    for (int i = 0; i < OD_NUM_FONTS; ++i)
    {
        if (!file_exists(FONT_FILES[i]))
        {
            fprintf(stderr, "error: could not find the font file '%s'\n", FONT_FILES[i]);
            ok = false;
        }
    }

    if (!file_exists(SHADERS_FOLDER CPU_GPU_COMMON_FILE))
    {
        fprintf(stderr, "error: could not find '%s'\n", SHADERS_FOLDER CPU_GPU_COMMON_FILE);
        ok = false;
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
        fprintf(stderr, "error: could not determine the size of '%s'\n", filename);
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
        fprintf(stderr, "error: failed to read '%s'\n", filename);
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
// can every rect pack into a (atlas_width x atlas_height) atlas ?
// this uses the exact same packing code path as the final bake, so a success here guarantees the bake succeeds
static bool can_pack_all(const stbrp_rect* rects, int num_rects, uint32_t atlas_width, uint32_t atlas_height, Arena* arena)
{
    stbtt_pack_context pack_context;

    memset(&pack_context, 0, sizeof(pack_context));
    if (!stbtt_PackBegin(&pack_context, NULL, (int)atlas_width, (int)atlas_height, 0, ATLAS_PADDING, NULL))
        return false; // out of memory

    stbrp_rect* copy = (stbrp_rect*)arena_alloc(arena, (size_t)num_rects * sizeof(stbrp_rect));
    bool packed = false;

    if (copy != NULL)
    {
        memcpy(copy, rects, (size_t)num_rects * sizeof(stbrp_rect));
        packed = stbrp_pack_rects((stbrp_context*)pack_context.pack_info, copy, (unsigned)num_rects) == 1;
    }

    stbtt_PackEnd(&pack_context);

    return packed;
}

// ---------------------------------------------------------------------------------------------------------------------------
// if the glyphs don't fit in a height, they don't fit in a smaller one either, so the search is monotonic
static uint32_t smallest_packing_height(const stbrp_rect* rects, int num_rects, uint32_t atlas_width, uint32_t min_height, Arena* arena)
{
    // the packer's target is (width-1) x (height-1) because of the padding, so the atlas must be at least 1 bigger than the biggest rect
    uint32_t low = min_height / 4;
    uint32_t high = ATLAS_MAX_SIZE / 4;

    if (!can_pack_all(rects, num_rects, atlas_width, ATLAS_MAX_SIZE, arena))
        return 0; // doesn't fit even at the maximum height

    while (high > low)
    {
        uint32_t mid = low + (high - low) / 2;

        if (can_pack_all(rects, num_rects, atlas_width, mid * 4, arena))
            high = mid;
        else
            low = mid + 1;
    }

    return high * 4;
}

// ---------------------------------------------------------------------------------------------------------------------------
// find the smallest atlas (width and height multiples of 4, at most ATLAS_MAX_SIZE) that fits every rect
static bool find_atlas_size(const stbrp_rect* rects, int num_rects, uint32_t* out_width, uint32_t* out_height, Arena* arena)
{
    uint32_t max_rect_width = 0;
    uint32_t max_rect_height = 0;

    for (int i = 0; i < num_rects; ++i)
    {
        if (rects[i].w > (int)max_rect_width)
            max_rect_width = (uint32_t)rects[i].w;

        if (rects[i].h > (int)max_rect_height)
            max_rect_height = (uint32_t)rects[i].h;
    }

    if (max_rect_width + 1 > ATLAS_MAX_SIZE)
    {
        fprintf(stderr, "error: a glyph is too wide (%u px) for an atlas of at most %u px, increase ATLAS_MAX_SIZE\n", max_rect_width, ATLAS_MAX_SIZE);
        return false;
    }

    struct size
    {
        uint16_t width;
        uint16_t height;
    };

    struct size sizes[ATLAS_MAX_SIZE / 4 + 1];
    size_t num_sizes = 0;
    float min_area = 1e30f;

    for (uint32_t width = round_up_4(max_rect_width + 1); width <= ATLAS_MAX_SIZE; width += 4)
    {
        uint32_t height = smallest_packing_height(rects, num_rects, width, round_up_4(max_rect_height + 1), arena);

        if (height == 0)
            continue; // this width is too narrow

        float area = (float)width * (float)height;
        if (area < min_area)
            min_area = area;

        sizes[num_sizes++] = (struct size){(uint16_t)width, (uint16_t)height};
    }

    if (num_sizes == 0)
    {
        fprintf(stderr, "error: could not pack the font faces in an atlas of at most %ux%u, increase ATLAS_MAX_SIZE or step back to the 7-size list\n",
                ATLAS_MAX_SIZE, ATLAS_MAX_SIZE);
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

// one (font file, pixel size) pair; the 4 fonts x OD_NUM_FONT_SIZES sizes
#define NUM_FACES (OD_NUM_FONTS * OD_NUM_FONT_SIZES)

// ---------------------------------------------------------------------------------------------------------------------------
static bool load_fonts(stbtt_fontinfo* fonts, Arena* arena)
{
    bool ok = true;

    for (int i = 0; i < OD_NUM_FONTS; ++i)
    {
        size_t file_size;
        uint8_t* data = read_file(FONT_FILES[i], &file_size, arena);

        if (data == NULL)
        {
            ok = false;
            continue;
        }

        if (!stbtt_InitFont(&fonts[i], data, 0))
        {
            fprintf(stderr, "error: '%s' is not a valid font file\n", FONT_FILES[i]);
            ok = false;
            continue;
        }

        for (int c = 0; c < NUM_CODEPOINTS; ++c)
        {
            if (stbtt_FindGlyphIndex(&fonts[i], (int)CODEPOINTS[c]) == 0)
            {
                fprintf(stderr, "error: '%s' is missing the glyph U+%04X\n", FONT_FILES[i], CODEPOINTS[c]);
                ok = false;
            }
        }
    }

    return ok;
}

// ---------------------------------------------------------------------------------------------------------------------------
// gather the packed rect of every code point of every face, in the order of the final pack
static void gather_face_rects(stbtt_fontinfo* fonts, stbrp_rect* rects)
{
    stbtt_pack_context pack_context;
    stbtt_pack_range range;

    memset(&pack_context, 0, sizeof(pack_context));
    stbtt_PackBegin(&pack_context, NULL, 1, 1, 0, ATLAS_PADDING, NULL);

    for (int f = 0; f < OD_NUM_FONTS; ++f)
    {
        for (int s = 0; s < OD_NUM_FONT_SIZES; ++s)
        {
            int face = f * OD_NUM_FONT_SIZES + s;

            memset(&range, 0, sizeof(range));
            range.font_size = FONT_SIZES[s];
            range.h_oversample = 1;
            range.v_oversample = 1;
            range.array_of_unicode_codepoints = (int*)CODEPOINTS;
            range.num_chars = NUM_CODEPOINTS;

            if (stbtt_PackFontRangesGatherRects(&pack_context, &fonts[f], &range, 1, &rects[face * NUM_CODEPOINTS]) != NUM_CODEPOINTS)
            {
                fprintf(stderr, "error: could not gather the glyph rects of face %d\n", face);
            }
        }
    }

    stbtt_PackEnd(&pack_context);
}

// ---------------------------------------------------------------------------------------------------------------------------
bool build_fonts(Arena* arena)
{
    stbtt_fontinfo fonts[OD_NUM_FONTS];

    fprintf(stdout, "opening fonts : ");
    if (!load_fonts(fonts, arena))
        return false;

    fprintf(stdout, "ok\n");

    fprintf(stdout, "sizing atlas : ");

    // one rect per baked code point per face (the arena does not zero its memory, so the unused
    // slots of a MAX_GLYPHS stride would reach the packer uninitialized)
    stbrp_rect* rects = (stbrp_rect*)arena_alloc(arena, (size_t)NUM_FACES * NUM_CODEPOINTS * sizeof(stbrp_rect));
    if (rects == NULL)
        return false; // out of memory

    gather_face_rects(fonts, rects);

    uint32_t atlas_width = 0;
    uint32_t atlas_height = 0;

    if (!find_atlas_size(rects, NUM_FACES * NUM_CODEPOINTS, &atlas_width, &atlas_height, arena))
        return false; // find_atlas_size reported the error

    size_t bc4_image_size = (atlas_width / 4) * (atlas_height / 4) * 8;

    fprintf(stdout, "%ux%u (%zu bytes of bc4 data)\n", atlas_width, atlas_height, bc4_image_size);

    fprintf(stdout, "baking %ux%u atlas : ", atlas_width, atlas_height);

    uint8_t* atlas_pixels = (uint8_t*)arena_alloc(arena, (size_t)atlas_width * atlas_height);
    if (atlas_pixels == NULL)
        return false; // out of memory

    memset(atlas_pixels, 0, (size_t)atlas_width * atlas_height);

    // the packedchar storage of every face (~1.8 MB, too big for the stack) is a plain array,
    // not part of the serialized layout : the baker reads it to fill the font bank
    stbtt_packedchar* bake_chars = (stbtt_packedchar*)arena_alloc(arena, (size_t)NUM_FACES * MAX_GLYPHS * sizeof(stbtt_packedchar));
    if (bake_chars == NULL)
        return false; // out of memory

    stbtt_pack_context pack_context;
    memset(&pack_context, 0, sizeof(pack_context));
    if (!stbtt_PackBegin(&pack_context, atlas_pixels, (int)atlas_width, (int)atlas_height, 0, ATLAS_PADDING, NULL))
        return false; // out of memory

    // pack every rect of every face in one pass, then render each face into its packed rects
    if (stbrp_pack_rects((stbrp_context*)pack_context.pack_info, rects, (unsigned)(NUM_FACES * NUM_CODEPOINTS)) != 1)
    {
        fprintf(stderr, "error: the font atlas (%ux%u) is too small to fit every face, increase ATLAS_MAX_SIZE or step back to the 7-size list\n",
                atlas_width, atlas_height);
        stbtt_PackEnd(&pack_context);
        return false;
    }

    for (int f = 0; f < OD_NUM_FONTS; ++f)
    {
        for (int s = 0; s < OD_NUM_FONT_SIZES; ++s)
        {
            int face = f * OD_NUM_FONT_SIZES + s;

            stbtt_pack_range range;
            memset(&range, 0, sizeof(range));
            range.font_size = FONT_SIZES[s];
            // must be set explicitly : render copies the oversampling from the range, and a zeroed range
            // renders every glyph at scale 0 (empty atlas) with infinite bearings
            range.h_oversample = 1;
            range.v_oversample = 1;
            range.array_of_unicode_codepoints = (int*)CODEPOINTS;
            range.num_chars = NUM_CODEPOINTS;
            range.chardata_for_range = &bake_chars[face * MAX_GLYPHS];

            stbtt_PackFontRangesRenderIntoRects(&pack_context, &fonts[f], &range, 1, &rects[face * NUM_CODEPOINTS]);
        }
    }

    stbtt_PackEnd(&pack_context);

    fprintf(stdout, "ok\n");

    fprintf(stdout, "compressing atlas in BC4 : ");

    uint8_t* compressed_atlas = (uint8_t*)arena_alloc(arena, bc4_image_size);
    if (compressed_atlas == NULL)
        return false; // out of memory

    bc4_encode(atlas_pixels, compressed_atlas, atlas_width, atlas_height);

    fprintf(stdout, "ok\n");

    fprintf(stdout, "packing the bc4 image with bc_packed : ");

    size_t packed_capacity = packed_bc4_maxsize(atlas_width, atlas_height);

    uint8_t* packed_atlas = (uint8_t*)arena_alloc(arena, packed_capacity);
    if (packed_atlas == NULL)
        return false; // out of memory

    bcp_context* bcp = bcp_init(NULL);
    if (bcp == NULL)
    {
        fprintf(stderr, "error: out of memory in bc_packed\n");
        return false;
    }

    uint32_t num_strips = 0;
    size_t packed_size = bcp_compress_bc4(bcp, compressed_atlas, atlas_width, atlas_height, packed_atlas, packed_capacity, &num_strips);
    bcp_terminate(bcp);

    if (packed_size == 0)
    {
        fprintf(stderr, "error: bc_packed compression failed (the atlas must be at least 16x256)\n");
        return false;
    }

    fprintf(stdout, "ok (%zu bytes, %.1fx smaller than the raw atlas, %.1fx smaller than the bc4 image)\n",
            packed_size, (float)((size_t)atlas_width * atlas_height) / (float)packed_size, (float)bc4_image_size / (float)packed_size);

    fprintf(stdout, "writting %s : ", FONT_H);

    if (!bin2h(FONT_H, "default_font_atlas", packed_atlas, packed_size))
        return false; // bin2h reported the error

    fprintf(stdout, "ok\n");

    fprintf(stdout, "filling the font bank : ");

    font_bank bank;
    memset(&bank, 0, sizeof(bank));
    bank.num_glyphs = (uint16_t)NUM_CODEPOINTS;
    bank.texture_width = (uint16_t)atlas_width;
    bank.texture_height = (uint16_t)atlas_height;

    for (int c = 0; c < NUM_CODEPOINTS; ++c)
        bank.codepoints[c] = CODEPOINTS[c];

    for (int f = 0; f < OD_NUM_FONTS; ++f)
    {
        int ascent = 0, descent = 0, line_gap = 0;
        stbtt_GetFontVMetrics(&fonts[f], &ascent, &descent, &line_gap);

        for (int s = 0; s < OD_NUM_FONT_SIZES; ++s)
        {
            int face = f * OD_NUM_FONT_SIZES + s;

            float scale = stbtt_ScaleForPixelHeight(&fonts[f], FONT_SIZES[s]);

            bank.faces[face] = (font_face)
            {
                .pixel_size = FONT_SIZES[s],
                .ascent = (float)ascent * scale,
                .line_height = (float)(ascent - descent + line_gap) * scale
            };

            for (int c = 0; c < NUM_CODEPOINTS; ++c)
            {
                const stbtt_packedchar* pc = &bake_chars[face * MAX_GLYPHS + c];
                bank.faces[face].glyphs[c] = (glyph)
                {
                    .x0 = (uint16_t)pc->x0,
                    .y0 = (uint16_t)pc->y0,
                    .x1 = (uint16_t)pc->x1,
                    .y1 = (uint16_t)pc->y1,
                    .bearing_x = pc->xoff,
                    .bearing_y = pc->yoff,
                    .advance_x = pc->xadvance
                };
            }
        }
    }

    if (bank.faces[0].line_height <= 0.f)
    {
        fprintf(stderr, "error: the font metrics are broken (line_height <= 0), check the vmetrics of '%s'\n", FONT_FILES[0]);
        return false;
    }

    fprintf(stdout, "ok\n");

    fprintf(stdout, "writting %s : ", GLYPH_H);

    if (!bin2h(GLYPH_H, "default_font", &bank, sizeof(font_bank)))
        return false; // bin2h reported the error

    fprintf(stdout, "ok\n");
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------------
bool prepare_shaders(Arena* arena, const char* metallib)
{
    fprintf(stdout, "\nwritting %s : ", SHADERS_H);

    size_t size = 0;
    void* binary = read_file(metallib, &size, arena);

    if (binary == NULL)
        return false; // read_file reported the error

    if (!bin2h(SHADERS_H, "shaders_metallib", binary, size))
        return false; // bin2h reported the error

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
int main(int argc, char** argv)
{
    fprintf(stdout, "onedraw %d.%d.%d library builder\n\n", ONEDRAW_MAJOR_VERSION, ONEDRAW_MINOR_VERSION, ONEDRAW_PATCH);

    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <metallib>\n", argv[0]);
        return 1;
    }

    if (!build_codepoints())
        return 1;

    if (!verify_inputs())
        return 1;

    Arena arena = {0};
    size_t peak_memory = 0;

    bool success = build_fonts(&arena);

    track_peak_memory(&arena, &peak_memory);
    arena_reset(&arena);

    success = success && prepare_shaders(&arena, argv[1]);

    track_peak_memory(&arena, &peak_memory);
    arena_reset(&arena);

    success = success && copy_files();

    fprintf(stdout, "\npeak memory usage %zukb\n\n", peak_memory >> 10);
    arena_free(&arena);

    if (success)
        fprintf(stdout, "build succeeded\n");
    else
        fprintf(stderr, "build failed\n");

    return success ? 0 : 1;
}
