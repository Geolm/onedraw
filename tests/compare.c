#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "compare.h"

#define REFERENCE_PSNR_THRESHOLD_DB (60.0f)

// ---------------------------------------------------------------------------------------------------------------------------
void test_bgra_to_rgba(uint8_t *pixels, uint32_t width, uint32_t height)
{
    uint32_t count = width * height;

    for (uint32_t i = 0; i < count; ++i)
    {
        uint8_t b = pixels[i * 4 + 0];
        pixels[i * 4 + 0] = pixels[i * 4 + 2];
        pixels[i * 4 + 2] = b;
    }
}

// ---------------------------------------------------------------------------------------------------------------------------
static const char* reference_name(uint32_t width, uint32_t height)
{
    return (width == 1280 && height == 720) ? "tests/references/primitives.png" : "tests/references/primitives_highdpi.png";
}

// ---------------------------------------------------------------------------------------------------------------------------
// The reference of a given size is the single source of truth for every test binary rendering
// that size, so the name is derived from the capture dimensions
void test_compare_reference(const uint8_t* pixels, uint32_t width, uint32_t height, bool compare, bool update_refs)
{
    const char* name = reference_name(width, height);

    uint32_t ref_width = 0, ref_height = 0, ref_channels = 0;
    uint8_t* reference = stbi_load(name, (int*)&ref_width, (int*)&ref_height, (int*)&ref_channels, 4);

    if (reference == NULL)
    {
        if (update_refs)
        {
            stbi_write_png(name, (int)width, (int)height, 4, pixels, (int)width * 4);
            fprintf(stdout, "reference : %s created from current capture\n", name);
        }
        else
            fprintf(stderr, "reference : %s not found, comparison skipped\n", name);
        return;
    }

    if (ref_width != width || ref_height != height)
    {
        fprintf(stderr, "reference : %s is %ux%u, screenshot is %ux%u, comparison skipped\n",
                name, ref_width, ref_height, width, height);
        stbi_image_free(reference);
        return;
    }

    double ssd = 0.0;
    uint32_t diff_pixels = 0, max_delta = 0;
    uint32_t pixel_count = width * height;

    for (uint32_t p = 0; p < pixel_count; ++p)
    {
        uint32_t c;
        bool pixel_differs = false;

        for (c = 0; c < 4; ++c)
        {
            int a = pixels[(p * 4u) + c];
            int b = reference[(p * 4u) + c];
            int delta = (a > b) ? (a - b) : (b - a);

            if (delta != 0)
            {
                pixel_differs = true;
                ssd += (double)delta * (double)delta;
            }

            if (delta > (int)max_delta)
                max_delta = (uint32_t)delta;
        }

        if (pixel_differs)
            diff_pixels++;
    }

    stbi_image_free(reference);

    if (ssd == 0.0)
    {
        fprintf(stdout, "reference : %s identical\n", name);
        return;
    }

    float psnr = (float)(10.0 * log10((255.0 * 255.0) / (ssd / (double)pixel_count / 4.0)));
    if (psnr >= REFERENCE_PSNR_THRESHOLD_DB)
    {
        fprintf(stdout, "reference : %s psnr %.1f dB (threshold %.1f) - ok\n", name, psnr, REFERENCE_PSNR_THRESHOLD_DB);
    }
    else
    {
        fprintf(stderr, "reference : %s psnr %.1f dB (threshold %.1f) - MISMATCH (%u/%u px differ, max delta %u)\n",
                name, psnr, REFERENCE_PSNR_THRESHOLD_DB, diff_pixels, pixel_count, max_delta);
        if (compare)
            exit(1);
    }

    if (update_refs)
    {
        stbi_write_png(name, (int)width, (int)height, 4, pixels, (int)width * 4);
        fprintf(stdout, "reference : %s updated from current capture\n", name);
    }
}
