/*
    svg — a static SVG layer on top of onedraw

    Parses a static subset of SVG 1.1 (shapes, paths, strokes, text) and replays it
    through the public onedraw API. Lives outside the library (extra/), CPU only.

    zlib License

    (C) 2026 Geolm

    This software is provided 'as-is', without any express or implied
    warranty.  In no event will the authors be held liable for any damages
    arising from the use of this software.

    Permission is granted to anyone to use this software for any purpose,
    including commercial applications, and to alter it and redistribute it
    freely, subject to the following restrictions:

    1. The origin of this software must not be misrepresented; you must not
    claim that you wrote the original software. If you use this software
    in a product, an acknowledgment in the product documentation would be
    appreciated but not required.
    2. Altered source versions must be plainly marked as such; they must not
    be misrepresented as being the original software.
    3. This notice may not be removed or altered from any source distribution.
*/

#ifndef __SVG_H__
#define __SVG_H__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <sv.h>

struct onedraw;

// All layer memory goes through this allocator : the layer never calls malloc, so the app
// can plug an arena (no per-object free), a heap, or a fixed buffer.
typedef struct svg_allocator
{
    void* (*alloc)(void* user_data, size_t size, size_t alignment);   // returns NULL when out of memory
    void  (*free)(void* user_data, void* ptr);                        // can be NULL for arenas
    void* user_data;
} svg_allocator;

// Bits of an svg_document's flags field : the document used features the layer degrades or skips,
// the app can use them to show a "degraded render" hint.
enum
{
    svg_flag_paint_unsupported  = 1 << 0,   // gradient / pattern paint, drawn with a solid color
    svg_flag_image              = 1 << 1,   // <image> elements skipped
    svg_flag_mask               = 1 << 2,   // masks ignored (unmasked)
    svg_flag_filter             = 1 << 3,   // filters ignored (unfiltered)
    svg_flag_clip_unsupported   = 1 << 4,   // a clipPath the layer can't map (unclipped) or nested clips
    svg_flag_stroke_unsupported = 1 << 5,   // a stroke the layer can't draw yet (path stroke, skewed stroke)
    svg_flag_text_degraded      = 1 << 6,   // text under a non-uniform transform, drawn unrotated
    svg_flag_oversized_piece    = 1 << 7,   // a flattened loop over 255 vertices, skipped
    svg_flag_content_dropped    = 1 << 8    // foreignObject / switch / animation / script / unknown element
};

typedef struct svg_document svg_document;
typedef struct svg_draw_list svg_draw_list;

// Parses [text] into a scale-independent document (XML, styles, transforms, paints, paths kept as curves).
// The document keeps no pointer into [text] (text content is copied), [text] can be freed after the call.
// Returns NULL on a parse error (malformed XML) or an allocation failure, nothing partial.
svg_document* svg_parse(String_View text, svg_allocator allocator);

// Frees the document back to its allocator.
void svg_free_document(svg_document* doc, svg_allocator allocator);

// Returns the size of the root svg/symbol in user units (width/height attributes, falling back to the
// viewBox, 100 x 100 when neither is set).
void svg_document_size(const svg_document* doc, float* width, float* height);

// Returns the document's flags field (svg_flag_*, 0 when everything is supported).
uint32_t svg_document_flags(const svg_document* doc);

// Prepares the document for a draw: flattens curves at the screen tolerance and lays out the commands in
// prepared space = user units x [scale] (the same space the onedraw calls of svg_draw() use, i.e. canvas
// units when the app set a canvas). [tolerance] is in prepared-space pixels (0.25 is a good default).
// Returns NULL on an allocation failure.
svg_draw_list* svg_prepare(svg_document* doc, float scale, float tolerance, svg_allocator allocator);

// Frees the draw list back to its allocator.
void svg_free_draw_list(svg_draw_list* list, svg_allocator allocator);

// The budget of the draw list : the number of commands svg_draw() will emit and the number of draw-data
// floats behind them, before any culling.
void svg_report(const svg_draw_list* list, uint32_t* num_commands, uint32_t* num_data_floats);

// Replays the draw list into [r] at offset [x, y] (prepared-space units, added to every coordinate).
// Commands whose box falls outside [0..viewport_width] x [0..viewport_height] (after the offset) are
// skipped, so a zoomed or panned document only pays for the visible part. No allocation.
// Must be called between od_begin_frame() / od_end_frame().
void svg_draw(struct onedraw* r, svg_draw_list* list, float x, float y, uint32_t viewport_width, uint32_t viewport_height);

#endif
