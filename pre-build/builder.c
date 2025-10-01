#include "file_buffer.h"
#include "bin2h.h"
#include "bc4_encoder.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#define UNUSED_VARIABLE(a) (void)(a)

#define FONT_FILENAME "../fonts/CommitMono-400-Regular.otf"
#define FONT_H "../lib/font.h"
#define FONT_CHAR_FIRST 33
#define FONT_CHAR_LAST 126
#define FONT_CHARS 95

// ---------------------------------------------------------------------------------------------------------------------------
bool build_font(float font_height, uint32_t atlas_width, uint32_t atlas_height)
{
    if ((atlas_width%4) != 0 || (atlas_height%4) != 0)
    {
        fprintf(stdout, "font atlas width and height must be a multiple of 4\n");
        return false;
    }

    fprintf(stdout, "opening font \'%s\' : ", FONT_FILENAME);

    size_t font_size;
    uint8_t* font_data = read_file(FONT_FILENAME, &font_size);

    if (font_data == NULL)
    {
        fprintf(stdout, "failed\n");
        return false;
    }

    fprintf(stdout, "ok\nbaking %dx%d atlas : ", atlas_width, atlas_height);

    uint8_t* atlas_pixels = malloc(atlas_height * atlas_width);
    if (atlas_pixels == NULL)
    {
        fprintf(stdout, "allocation failed\n");
        free(font_data);
        return false;
    }

    stbtt_bakedchar glyphs[FONT_CHARS];
    int result = stbtt_BakeFontBitmap(font_data, 0, font_height, atlas_pixels, atlas_width, atlas_height,
                                      FONT_CHAR_FIRST, FONT_CHARS, glyphs);

    if (result == 0)
    {
        fprintf(stdout, "failed\n");
        free(atlas_pixels);
        free(font_data);
        return false;
    }
    else if (result < 0)
    {
        uint32_t num_chars = -result;
        fprintf(stdout, "warning only %d chars fit\n", num_chars);
    }
    else fprintf(stdout, "ok\n");

    fprintf(stdout, "compressing atlas in BC4 : ");

    size_t bc4_image_size = (atlas_width/4) * (atlas_height/4) * 8;
    uint8_t* compressed_atlas = malloc(bc4_image_size);

    if (compressed_atlas == NULL)
    {
        fprintf(stdout, "allocation failed\n");
        free(font_data);
        free(atlas_pixels);
        return false;
    }

    bc4_encode(atlas_pixels, compressed_atlas, atlas_width, atlas_height);

    fprintf(stdout, "ok\nwritting %s : ", FONT_H);

    bool success = bin2h(FONT_H, "default_font", compressed_atlas, bc4_image_size);
    if (!success)
        fprintf(stdout, "failed\n");
    else
        fprintf(stdout, "ok\n");

    free(compressed_atlas);
    free(atlas_pixels);
    free(font_data);
    return success;
}


// ---------------------------------------------------------------------------------------------------------------------------
int main(int argc, const char * argv[]) 
{
    UNUSED_VARIABLE(argc);
    UNUSED_VARIABLE(argv);

    if (!build_font(32.f, 256, 256))
        return -1;

    return 0;
}
