# onedraw — a GPU-driven 2D renderer

## Canvas

The **canvas** is a logical drawing space — a `(x, y, width, height)` rectangle — mapped onto the framebuffer with a **uniform scale**:

```
framebuffer_pos = fmaf(canvas_pos, s, (x, y))
s = framebuffer_width / canvas_width
```

Every `od_draw_*` position is mapped and every distance (radius, thickness, width, roundness, …) is scaled **on the CPU** before the commands reach the GPU buffers — the whole pipeline (shaders, buffer layouts, quantized AABBs, tile cache) keeps working in framebuffer pixels, so nothing on the GPU changes. The [1440p example in the regression test](../tests/test.c) uses exactly this: a 2560×1440 canvas is an identity transform on a 2× display and a 0.5 scale on a 1× one, so the scene is laid out at a fixed design resolution on both.

With the default canvas — the framebuffer size, set in `od_init()` — the scale is 1.0 and the origin is (0, 0): a 1.0 multiply-add that is an exact IEEE no-op, so the output is bit-identical to not having a canvas.

### The ratio must match

A non-uniform scale would turn discs into ellipses in the SDFs, so the canvas always has the framebuffer's aspect ratio. If the `width`/`height` passed to `od_set_canvas()` doesn't match the framebuffer ratio, **the height is recomputed from the width** (`width * framebuffer_height / framebuffer_width`, a one-shot log records the adjustment) — the width is kept, nothing is rejected, and the scale stays uniform. The height re-derives again on `od_resize()`; the scale itself is recomputed from the current framebuffer width at every `od_begin_frame()`.

### The canvas is not a clip

The canvas is a coordinate mapping: content that falls outside the framebuffer (for instance a non-zero origin) is simply cropped at the framebuffer edge, exactly like out-of-bounds shapes are today, and the area the canvas doesn't cover shows the clear color.

### Clips: one push variant per space

Internally every clip lives in framebuffer pixels (the clip buffer, the 16-deep stack and the 256-shape budget are shared, and the tile cache hashes the clip values), but there is a push variant per space:

* **`od_push_cliprect()` / `_rounded` / `od_push_clipdisc()`** (the original names) — like scissor rects in any graphics API: framebuffer pixels, not affected by the canvas. You place a clip where you want it *on screen*, and the canvas-scaled geometry is clipped against it.
* **`od_canvas_clip*()`** — the coordinates and radius are mapped by the canvas transform, so the clip **follows the canvas-scaled shapes**. This is the natural choice when the scene is designed in canvas units (as the [regression test](../tests/test.c) does).

### Text lives in canvas units

`od_draw_text()` takes its position and its `size` in canvas units, like every other primitive: a label follows the canvas-scaled scene, and the same call renders the same relative size at any resolution. Internally the library converts the size to physical pixels (`size * canvas scale`) to pick the closest baked face, so a size of `16` on a 2x display bakes to the 32 px face at scale 1.0.

The measurements `od_text_width()`/`od_text_ascent()`/`od_text_line_height()` are returned **in canvas units** (the physical value divided by the canvas scale), so text layout math — centering, aligning, stacking labels — stays resolution independent.

### Tile cache

A canvas change changes the data floats and the quantized AABBs of every mapped command, which changes every content hash: a full redraw, no extra work needed. An identity canvas changes nothing, so the cache behaves exactly as before.
