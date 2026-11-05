#ifndef __SCENE_H__
#define __SCENE_H__

#include <stdint.h>

#include "../lib/onedraw.h"

#define CANVAS_WIDTH (2560.f)
#define CANVAS_HEIGHT (1440.f)
#define TEX_SIZE (256)

#define FROM_HTML(html)   ((html&0xff)<<16) | ((html>>16)&0xff) | (html&0x00ff00) | 0xff000000

// https://lospec.com/palette-list/miyazaki-16
extern const uint32_t miya_black;
extern const uint32_t miya_dark_blue;
extern const uint32_t miya_dark_grey;
extern const uint32_t miya_grey;
extern const uint32_t miya_light_grey;
extern const uint32_t miya_pale_blue;
extern const uint32_t miya_white;
extern const uint32_t miya_blue;
extern const uint32_t miya_light_blue;
extern const uint32_t miya_brown;
extern const uint32_t miya_red;
extern const uint32_t miya_pink;
extern const uint32_t miya_dark_green;
extern const uint32_t miya_green;
extern const uint32_t miya_light_green;
extern const uint32_t miya_yellow;

// Sets up the renderer for the test scene : clear color, canvas and the texture array upload
void scene_init(struct onedraw* r);

// Draws the whole scene : the moving elements are derived from [frame_count] so their draw data
// changes every frame and the tile cache must see the changed command hashes
void scene_draw(struct onedraw* r, uint32_t frame_count);

#endif
