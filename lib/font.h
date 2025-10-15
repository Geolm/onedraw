#ifndef __FONT_H__
#define __FONT_H__

#include <stdint.h>

#define MAX_GLYPHS (128)

struct glyph
{
    uint16_t x0, y0, x1, y1;
    float bearing_x, bearing_y;
    float advance_x;
};

struct alphabet
{
    struct glyph glyphs[MAX_GLYPHS];
    float font_height;
    uint16_t num_glyphs;
    uint16_t first_glyph;
    uint16_t texture_width;
    uint16_t texture_height;
};

#endif