#ifndef __SDF2D_H__
#define __SDF2D_H__

#include <stdint.h>

#define SDF2D_FONT_MAX_CHARS (128)

typedef struct 
{
    uint16_t x0, y0, x1, y1;
    float bearing_x, bearing_y;
    float advance_x;
} font_glyph_t;

typedef struct
{
    font_glyph_t glyphs[SDF2D_FONT_MAX_CHARS];
    uint16_t num_glyphs;
    uint16_t first_glyph;
    uint16_t texture_width;
    uint16_t texture_height;
} font_t;



#endif