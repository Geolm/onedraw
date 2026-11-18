# SVG rendering with onedraw

What SVG content onedraw can draw, what is cheap to add, and what is out of question. Scope: static SVG 1.1 /
SVG Tiny style content (icons, logos, badges, charts, simple illustrations). No animation, no scripting, no CSS engine.

onedraw is not an SVG library and doesn't become one. The work splits in two parts with a strict boundary:

* [Part A: the svg layer](#part-a-the-svg-layer-extra-cpu-only): in `extra/`, outside the library, its own naming
  (`svg_*`, no `od_` prefix). Everything SVG lives there: parsing, styles, transforms, paths, curves, fill rules,
  strokes, dashes, text mapping, and **untangling** every path into clean geometry onedraw can draw: small simple
  polygons (holes bridged). It only calls the public onedraw API.
* [Part B: onedraw features](#part-b-onedraw-features-library): generic drawing features, useful to any app, in
  onedraw's terms, with no SVG concept in the API: `od_draw_polygon` (B1, shipped), a draw data capacity option (B2,
  deferred).

## Status (2026-11-18)

| Item                              | State                                                                                         |
|-----------------------------------|-----------------------------------------------------------------------------------------------|
| B1 `od_draw_polygon`              | **Shipped in v3.7.0** (current: 3.7.1). See [B1 as shipped](#b1-od_draw_polygon--shipped-record). |
| B2 draw data capacity             | **Deferred** until a scene needs it (spec kept in [B2](#b2-draw-data-capacity--deferred)).    |
| Part A (the svg layer)            | Not started, no `extra/` yet. This plan.                                                      |
| B1 GPU profiling run              | Not done. Proceed with the 32 px piece default, retune after A6 (see [A3](#a3-untangling-any-path-any-fill-rule--700-loc)). |

## Decisions (2026-11-18)

* Piece/cell size: start at 32 px (2×2 tiles), retune later from the A6 histogram + a GPU profiling run.
* B2: defer; add it only when the corpus shows a draw-data shortage.
* `sv.h` (tsoding/sv, MIT): approved for vendoring in `third_party/`.
* First milestone: core A0 + A1 + A2 + A3 + A4. `<image>` and the canvas shortcut stay in the plan as later CLs.
* Test: a dedicated `svg` test binary (not a `scene.c` section). Fixed set of files (default: all of
  `tests/svg/*.svg`), one fixed 1280×720 offscreen render, one reference (`tests/references/svg.png`);
  window mode at the machine's DPI for eyeballing, no comparison there.

## Fidelity

* **Edge position**: coverage is 1 at `d <= 0` and ramps to 0 at `d = aa_width` (`√2` px, from the rasterizer), so every
  shape is ~0.5 px fatter than a browser/resvg rasterization. Fine for viewing, visible in pixel diffs against resvg.
* **Blending space**: colors are decoded to linear, blended in linear, re-encoded. Browsers blend translucent layers and
  interpolate gradients in sRGB (`color-interpolation: sRGB` is the default). Translucent overlaps look different, and
  cannot match without a deeper renderer change.
* **No layer compositing**: only one group at a time (groups don't nest in onedraw), no offscreen layers, so there is
  no way to composite a subtree.

### Levers in the svg layer

* **Known cost**: `svg_prepare` reports the exact commands and draw data floats of the draw list (polygons,
  primitives, glyphs, groups), so the app can budget it (simplify, placeholder).
* **Flattening tolerance** in screen pixels from the final scale, 0.25 px by default (onedraw's own bezier strokes use
  0.1 px, too fine for documents), recomputed when the zoom changes by more than ~2x. Fewer vertices: fewer pieces,
  faster untangling.
* **Polyline simplification** (Douglas-Peucker at the same screen tolerance) before untangling, for input already
  denser than the pixels: maps, plots with thousands of points. A 100k-vertex coastline drawn 1000 px wide needs a few
  thousand edges.
* **Culling**: onedraw does not cull command emission. The layer culls in `svg_draw`: every piece whose offset box is
  outside the viewport or the active clip is skipped, so zoomed-in documents only pay for the visible part.
* **Level of detail**: elements whose screen box is under ~0.5 px are skipped, strokes thinner than ~0.1 px too.
* **Text**: `od_draw_string_view` already emits no command for empty glyphs (spaces), so the cost is the visible
  glyphs; labels smaller than ~4 px on screen are skipped or drawn as a thin bar.

---

# Part A: the svg layer (extra, CPU only)

Everything here only calls the public onedraw API: no shader, no GPU resource, no change in `onedraw.cpp`, no SVG
concept leaking into onedraw. It lives in `extra/svg.c/.h` (C, compiles under the project's existing C flags), with its
own prefix (`svg_*`), and is compiled into the new `svg` test target (A6); an app links it if it wants SVG, it is not
part of the library. No version bump for any Part A change.

## A0. Architecture

* Memory through an allocator interface given by the caller, so the app plugs its arena, its heap or a fixed buffer;
  the layer never calls `malloc`:

  ```c
  typedef struct svg_allocator
  {
      void* (*alloc)(void* user_data, size_t size, size_t alignment);   // returns NULL when out of memory
      void  (*free)(void* user_data, void* ptr);                       // can be NULL for arenas
      void* user_data;
  } svg_allocator;
  ```

  No `realloc`: every array is sized by a counting pass first (parse the text twice, it is cheap next to the rest), so
  each array is one allocation and an arena never wastes grown copies. The untangler's working arrays have bounds
  known after the intersection pass (A3). Out of memory returns an error, nothing partial. The tests use the existing
  `third_party/arena.h`.
* Three steps:
  * `svg_parse(text, allocator) -> document`: XML, styles, transforms, paints, paths kept as curves. Scale
    independent, once per file. `text` is a `String_View`.
  * `svg_prepare(document, scale, allocator) -> draw list`: flattening and simplification at the screen tolerance,
    stroke outlines (A4), untangling and pieces (A3). All geometry lands in **prepared space** = user units × `scale`
    (device pixels at the intended display); the tolerance and the snap grid are in prepared-space pixels.
  * `svg_draw(onedraw, draw list, x, y, viewport)`: per frame, no allocation. Applies the `(x, y)` device-pixel
    offset to the pre-transformed pieces, culls against the viewport and the active clip, replays with onedraw calls.
    A pan is a free offset; only a scale change (zoom) re-prepares, and only when it moves by more than ~2x.
  * `svg_free` of a document / draw list goes back to the allocator.
* Strings: [sv.h](https://github.com/tsoding/sv) (`String_View`, MIT, no allocation), vendored in `third_party/sv.h`
  (approved), `SV_IMPLEMENTATION` in `svg.c`. The input text, tokens, attribute values and the svg API strings are
  `String_View`s. Everything is resolved during the parse (numbers, colors, transforms, `id` / `class` / `href` /
  `<style>`), except the `<text>` content, which is copied into the document with entities decoded and whitespace
  collapsed: the document keeps no pointer to the caller's text, which can be freed after `svg_parse`. sv.h only
  parses integers (`sv_to_u64`): numbers (`-1.5e-3`, `.5` in path data) need a small float parser on a `String_View`
  in the layer (`strtod` needs a terminated string).
* XML: a minimal hand-written tokenizer on top of sv.h (elements, attributes, `style="a:b;..."`, entities, no DTD),
  ~250 LoC.
* Transforms are applied on the CPU in `svg_prepare`. Translate, rotate and **uniform** scale keep every primitive
  exact (all of them are oriented: circles stay circles, ellipses / boxes / capsules / triangles rotate). Non-uniform
  scale and skew: circle -> ellipse (exact), ellipse -> ellipse (new axes from the 2x2 matrix, exact), rect -> polygon
  (exact, corner radii lost); strokes outlined before the transform (A4) are exact too.
* `viewBox` + `width` / `height` + `preserveAspectRatio` (meet / slice / none, the 9 alignments): the viewBox ->
  viewport matrix, folded into the CPU transform stack. Nested `svg`, `symbol` and `marker` push their own viewBox the
  same way. Overflow is clipped to the viewport by default: `od_canvas_cliprect` on the viewport box (the canvas is not
  a clip).
* Full-screen shortcut (later CL): when the SVG is the only thing on screen, the canvas (`od_set_canvas`) can do the
  root mapping. The canvas keeps its width, recomputes its height from the framebuffer ratio and is anchored
  top-left, so pick its rect as the viewBox grown to the framebuffer ratio: for `meet` grow the short side and use
  negative `x` / `y` to center (or align) the content, for `slice` shrink it instead. `none` cannot be done (uniform
  scale only). Text scales with the canvas (`size * canvas.scale` picks the face), so the shortcut works for text too.
  The canvas is a per-frame global shared with every other draw, so an embedded SVG (an icon in a UI) always uses the
  CPU matrix; the shortcut is an option, not something the layer relies on.
* `g`, `use`, `defs`, `symbol`: style inheritance, transform concatenation, instancing. `<style>` with plain type /
  class / id selectors, anything else ignored.

## A1. Mapping

| SVG                                   | onedraw mapping                                                                 |
|---------------------------------------|---------------------------------------------------------------------------------|
| `circle` fill / stroke                | `od_draw_disc` / `od_draw_ring`                                                 |
| `ellipse` fill / stroke               | `od_draw_ellipse` / `od_draw_ellipse_ring`                                      |
| `rect` (with `rx == ry`) fill / stroke| `od_draw_box` / `od_draw_oriented_rect`, rotated: `od_draw_oriented_box`        |
| `line`                                | `od_draw_line` (butt), `od_draw_capsule` (round), square cap = line extended by w/2 |
| `polygon`, `path` fill                | untangled and cut into pieces (A3), `od_draw_polygon` (B1, `roundness = 0`)      |
| `polyline`, `polygon`, `path` stroke  | see A4                                                                          |
| path curves                           | `C`/`S`/`Q`/`T`/`A` flattened by the layer at the screen tolerance              |
| `fill` / `stroke` color, `*-opacity`, `opacity` on a leaf | `draw_color` alpha (CSS named colors, `#rgb`, `#rrggbb`, `rgb()`, `currentColor`) |
| `stroke-dasharray` with 1-2 values on `line`, `circle`, `rect`, `ellipse` | the `od_draw_dashed_*` functions (`od_dash_style`, `round_caps` from `stroke-linecap`), `stroke-dashoffset` = offset |
| `clipPath` with a single `rect` / rounded `rect` / `circle` | `od_canvas_cliprect(_rounded)` / `od_canvas_clipdisc`; one clip level per onedraw push (see A5) |
| `text` / `tspan`                      | see [A2](#a2-text)                                                              |
| `image` (PNG / JPG, embedded base64 or file) — later CL | `stb_image` (already vendored) decode, `od_upload_region` into the atlas, `od_draw_quad` / `od_draw_oriented_quad` |

## A2. Text

Partial, with onedraw's built-in font bank:

* `<text>` / `<tspan>` with `x` / `y` and `dx` / `dy`, drawn with `od_draw_text` / `od_draw_string_view`.
* Font selection: `font-weight: bold` -> bold, `font-style: italic` -> italic, monospace families -> mono, any other
  `font-family` -> regular.
* `font-size`: the size is in canvas units; onedraw picks the closest baked size (9 sizes, 12–64 px) >= the physical
  size and minifies it. Above 64 px it magnifies the 64 px bake, so very large titles get soft.
* `text-anchor` (start / middle / end) via `od_text_width` / `od_string_view_width`; the SVG baseline converted to
  onedraw's top-left anchor with `od_text_ascent`.
* Solid `fill` color and `fill-opacity`; translation and uniform scale on the position and the size.
* Characters: ASCII, Latin-1, common typography, a few math symbols and arrows (234 glyphs).
  Unknown code points advance by a space width.

Not supported (see [out of question](#out-of-question)): rotated / skewed text, custom fonts, `textPath`,
`letter-spacing`, `word-spacing`, `textLength`, bidi / shaping, text `stroke`, gradient text, text as a clip. The
layout does not match the browser either: the fallback families have other metrics than the intended font, so text
placed with absolute `x` positions or fitted into boxes can overlap or leave gaps (the main visual risk for exported
charts and diagrams). Text already converted to paths by the exporter (matplotlib, PDF converters) is a regular path
fill and renders exactly.

## A3. Untangling: any path, any fill rule (~700 LoC)

Input: the flattened loops of one path (each subpath closed implicitly for the fill) and its fill rule. Output: the
exact filled area as **regions**, each a simple outer loop with its holes, regions disjoint (touching at points at
most). No self-intersection, no overlap, no internal edge, so no input is "approximated".

* **Snap to an integer grid** in prepared space (1/64 px, `int32` coordinates, `int64` orientation predicates): exact
  predicates, no epsilon anywhere. Zero-length edges and consecutive duplicates dropped. (Screen space is bounded by
  the 4096 px max, so 4096 × 64 fits `int32` with room.)
* **Intersections**: all edge pairs that cross, found with the edges bucketed on a coarse grid (pairs tested only
  within a bucket), or a Bentley-Ottmann sweep if profiling asks for it. **Snap rounding** (Hobby): intersection points
  rounded to the grid, and every edge passing through a rounded point's grid pixel is split there, so the rounded
  arrangement keeps the topology and no new crossing appears.
* **Planar graph**: edges split at the snapped points, coincident edges merged with the sum of their winding deltas
  (+1 / -1 per direction), half-edges sorted by angle around each vertex, faces traced.
* **Winding per face**: from the unbounded face (0), across each edge by its delta (a BFS over the faces).
* **Fill rule**: `nonzero` keeps faces with winding != 0, `evenodd` the odd ones.
* **Outline**: the half-edges between a kept and a dropped face, oriented with the kept face on the left, chained into
  loops; outer loops and holes told apart by orientation, holes assigned to the smallest enclosing outer loop.
  Collinear vertices removed.
* **Pieces for `od_draw_polygon`**: onedraw culls a polygon by its AABB only and every pixel of the tiles under it
  evaluates all its edges, so the layer cuts the filled area into small polygons. The lines of a grid of cells are
  added to the planar graph as edges with no winding delta, so every kept face lies in one cell; the faces of a cell
  form its pieces, the full cells of a row are merged into one rectangle. A piece over 255 vertices is cut again along
  a finer grid (the `OD_MAX_POLYGON_VERTICES` limit). **Cell size: 32 px (2×2 of the 16 px tiles) to start**; the
  A6 histogram on the corpus plus a GPU profiling run (user-run, per AGENTS.md) picks the final size — it is a
  constant, cheap to retune.
* **One closed loop per piece** (parity): each hole joined to its outer loop by a bridge (earcut's hole elimination:
  the hole's rightmost vertex to a visible outer vertex), traversed both ways. The two bridge edges are identical, so
  they cancel in the parity, and they lie inside the filled area, where their distance has no visible effect.
  Adjacent pieces share edges on the cell lines: a translucent path is drawn in a group so their AA doesn't double
  (onedraw groups fold the shapes into one SDF and composite once). `od_draw_polygon` is passed `roundness = 0`; its
  0.5 px vertex dedup is a no-op for snapped, simplified output.
* Cost: once per `svg_prepare`, linear in the edges plus the intersections in practice. A 10k-edge path with a few
  hundred crossings is well under a millisecond of CPU per zoom step; measured on the corpus.
* The alternative is **libtess2** (C, custom allocator interface, winding rules, outputs boundary contours): exactly
  this job, ~3k LoC, but a third-party dependency that needs approval, and float-based (known to
  struggle on degenerate input, where snap rounding doesn't).

## A4. Strokes (~200 LoC + the untangler)

* The stroker emits a **naive outline**, and the untangler (A3, `nonzero`) turns it into clean regions:
  * one rectangle per segment (`w / 2` each side), all wound the same way;
  * per join, per `stroke-linejoin`: the miter quad within `stroke-miterlimit` (else bevel), the bevel triangle, or a
    flattened round wedge;
  * per open subpath end, per `stroke-linecap`: butt (nothing), square (a w/2 rectangle), round (a flattened half
    circle).
  * The pieces overlap freely: the union under `nonzero` removes every overlap, inner join and self-crossing, so the
    stroker has no special case, no double blending when translucent, and no internal edge.
* `stroke-dasharray` with any number of values on any flattened path: a polyline dash walker in the layer (~60 LoC),
  the dashes outlined like subpaths, all in the same untangling. (1–2 value arrays on the primitive shapes skip the
  walker and use the `od_draw_dashed_*` functions directly, A1.)
* Strokes are outlined in user space, then transformed: non-uniform scale and skew give the exact stroke shape.
* `vector-effect: non-scaling-stroke`: outline after the transform instead.
* A path with both fill and stroke: two separate untanglings (the stroke is painted over the fill, as in SVG).

## A5. Small items (each < 50 LoC)

* `rect` with `rx != ry`: as a path fill (A3).
* `marker-start/mid/end`: instancing of the marker content at the path vertices with the tangent rotation.
* `opacity` on a `g` whose children don't overlap (common: a translucent icon): multiplied into the children. Wrong
  where children overlap; documented.
* `display="none"`, `visibility`, `fill="none"`, `stroke="none"`: skipped.
* A blurred `rect` shadow (`feGaussianBlur` on a single rect): `od_draw_blurred_box`.
* **Clips**: one `clipPath` per level, mapped to a single `od_canvas_clip*` push. Nested clip paths (a clip inside a
  clipped group) are unsupported: onedraw pushes replace the parent clip (no intersection) and the clip stack is 16
  deep.
* **Unsupported content**: the element or attribute is ignored and the element drawn as if it were absent (a mask:
  unmasked, a filter: unfiltered, a `clipPath` the layer can't map: unclipped, rotated text: unrotated at its
  position); `foreignObject`, `switch`, animation and scripts are dropped. Each unsupported feature sets a bit in a
  flags field of the document, so the app knows the render is degraded.
* **Unsupported paint** (gradient, pattern): the SVG fallback color (`fill="url(#p) red"`) when given, else the
  average of the stops for a gradient, `none` for a pattern.

## A6. Testing

A dedicated `svg` test binary (its own CMake target, sources: `onedraw.cpp`, `extra/svg.c`, `tests/svg_test.c`,
`sokol_app.mm`, `compare.c`), separate from `test` / `headless`. It reads SVG files, takes a screenshot and compares
it to a reference using the existing test functions (`od_take_screenshot`, `test_bgra_to_rgba`, `stbi_write_png`,
`test_compare_reference`); no `scene.c` change, the existing `primitives*.png` references stay untouched.

* **Input**: one or more SVG files as positional arguments. With no arguments it renders the **fixed set** = every
  `tests/svg/*.svg` in name order (the deterministic regression set). Fixture files live in `tests/svg/` (an icon, a
  badge, a chart, a text-as-paths chart, a text-heavy diagram); read at init from `tests/svg/<name>.svg`, falling
  back to `../tests/svg/<name>.svg` (running from the repo root vs from the build dir); a missing file fails the test.
* **Two modes** (same shape as `test.c`):
  * `--offscreen`: renders into the offscreen target at a **fixed 1280×720** — the viewport is set once and window
    resize events are ignored, so the capture is identical on every machine/DPI (no highdpi/lowdpi fork). This is the
    only mode that screenshots and compares.
  * default (windowed): the SVG is rendered in a window at the machine's DPI for eyeballing, no screenshot, no
    comparison.
* **Reference**: one capture per run, one reference — `tests/references/svg.png` (1280×720) — since the fixed set and
  the layout are stable. `test_compare_reference` derives its name from the capture size, so the named reference
  needs a small addition in `compare.c`: `test_compare_reference_named(pixels, w, h, name, compare, update_refs)`,
  with the existing function delegating to it (no behavior change for `test` / `headless`). Regenerated with
  `--update-refs` in the same CL that changes the set or the rendering.
* **Layout**: a fixed grid in the 1280×720 frame — each file fitted in its cell with a fixed margin, a fixed clear
  color, files in name order. Deterministic, so the reference is stable.
* **Flags**: `--offscreen`, `--compare`, `--update-refs`, `--validate` (as in `test.c`); `--compare` /
  `--update-refs` without `--offscreen` is a hard error. The CPU checks below run in both modes at start.
* **Untangling (CPU checks)**: the one-loop star in `nonzero` (center filled) and `evenodd` (center empty), two
  overlapping circles in one path (same and opposite orientation, both rules), a glyph with two holes (`B`), nested
  regions (a ring inside a ring), a figure-eight, coincident and collinear overlapping edges, a vertex touching an
  edge, many loops through one point, a degenerate zero-area loop, a path with thousands of crossings (a scribble).
  The output checked for no crossing and for its area against the input's winding. Run at test start, exit non-zero
  on failure (product-code assertions, not harness tests).
* **Budget**: the cost reported by `svg_prepare` matches the commands and floats `svg_draw` emits; culling and
  simplification lower the count of a zoomed / dense document; an allocator that fails mid-untangle returns an error
  without leaks.
* **Unsupported content**: a file with a mask, a filter, a pattern with and without a fallback color, and rotated text
  draws the rest of the document and reports the matching flags.
* **Strokes**: every cap x join combination, miter limit fallback, a closed subpath, a spiral, a wide stroke on a tight
  curve, a self-crossing stroke, a multi-value dash array, a translucent stroke (no double blending anywhere), a skewed
  stroke.
* **Pieces**: no seam at the cell lines, opaque and translucent; the vertices-per-piece histogram on the corpus for a
  few cell sizes, reported by the test, for the A3 retune.
* **Gates**: every CL runs `./build/test --autoexit --compare` and `--validate` (no Metal validation errors), and
  `./build/svg --offscreen --compare --validate`. CI adds a `./build/svg --offscreen --compare --validate` step to
  both debug and release jobs once the first `svg.png` reference lands.

---

# Part B: onedraw features (library)

Generic features, motivated by SVG but designed for any app: no path, no fill rule, no stroke style in the API. Each
item is one CL with its regression scene, `--compare` and `--validate`, and none depends on Part A.

## B1. `od_draw_polygon` — shipped (record)

Shipped in v3.7.0. A filled polygon: one closed loop of vertices, iq's `sdPolygon` (ray-crossing parity) in
`rasterizer.metal`. Concave and self-intersecting loops are fine; holes and multiple parts are the caller's
arrangement.

```c
void od_draw_polygon(struct onedraw* r, const float* vertices, uint32_t num_vertices, float roundness, draw_color srgb_color);
```

As shipped (deviations from the original draft, for the record):

* `OD_MAX_POLYGON_VERTICES` (255) in the header; the count rides in the command's 8-bit `extra`, like the glyph index.
* A `roundness` parameter shrinks the shape to round the corners (like `od_draw_triangle`); the svg layer passes 0.
* CPU dedup: vertices closer than half a pixel to the previous kept one are dropped (keeps every GPU edge ≥ 0.5 px,
  so `sd_polygon` never divides by a zero-length edge); a loop reduced to 3 vertices is drawn as a triangle, fewer
  than 3 log and draw nothing.
* Draw data: `2 * num_vertices + 1` floats (the vertices, then the roundness), transformed by the canvas
  (`canvas_xform_pos`).
* Culling: AABB only (grown by `draw_cmd_aabb_bump`), nothing finer. Works inside a group like any other shape.

Pending: the before/after GPU profiling run and a polygon scene at several vertex counts/sizes (user-run), to confirm
register pressure of `tile_fs` and to retune the A3 cell size.

## B2. Draw data capacity — deferred

Trigger: a corpus document whose `svg_prepare` report approaches the 262144-float budget (the data buffer is shared
by every primitive in the frame; ~8000 pieces of ~12 vertices are ~190k floats, over half of it).

When needed:

* `onedraw_def::draw_data_capacity`: number of floats of the draw data buffer, 0 keeps `MAX_DRAWDATA` (262144). The
  buffer is a plain shared buffer indexed by the 32-bit `data_index`, nothing else depends on its size.
* Cost: 4 bytes per float per in-flight copy (the `DynamicBuffer` keeps two buffers), so +2 MiB per extra 262144
  floats; reported by `od_get_stats().gpu_memory_usage` as today.
* It removes data as a limit for large scenes, leaving only the 65536 commands. Minor bump (3.8.0).
* Test: init with a larger capacity, a scene over the default data size draws completely; `--validate`.

---

## Out of question

| Feature                                   | Why                                                                                     |
|-------------------------------------------|-----------------------------------------------------------------------------------------|
| `mask`, luminance / alpha masks           | needs an offscreen layer and a second composite; onedraw renders a tile list in one pass |
| `opacity` on a `g` with overlapping children, `isolation` | same: layer compositing                                                  |
| `filter` (`feGaussianBlur`, `feDropShadow`, `feColorMatrix`, `feTurbulence`, ...) | image-space effects need layers; the only blur is the analytic blurred box (A5) |
| `mix-blend-mode`, `feBlend`               | blending is a fixed over in the fragment shader                                          |
| general `clipPath` (paths, text, several shapes, nested / intersected clips) | clips are rect / disc only, a push replaces instead of intersecting, 256 per frame, 16 deep. A path clip is an intersection of regions, which the untangler could compute for fills, but not for text, images or strokes drawn as primitives |
| `pattern`                                 | needs a tiled render of arbitrary content; the atlas has no tiling / mirror, quads no wrap |
| `linearGradient`, `radialGradient` (and conic / mesh gradients) | not in onedraw; drawn with a solid color (A5) |
| custom fonts, rotated / skewed text, `textPath`, text stroke, `letter-spacing`, `textLength`, bidi / shaping | the font bank is baked at build time (9 sizes, 234 glyphs), glyphs are axis-aligned screen quads. Text to polygons needs a font loader and outlines, i.e. a second font pipeline |
| SVG concepts in onedraw (paths, fill rules, stroke styles, curves on the GPU) | onedraw stays a generic renderer |
| SMIL animation, scripting, `foreignObject`, CSS beyond simple selectors, `switch` / `systemLanguage` | not a renderer topic; the layer stays a static subset |
| embedded SVG images (`image href="x.svg"`) | recursion of the layer; cheap to add later, no demand |
| exact match with browsers / resvg         | edge offset (~0.5 px) and linear-space blending are by design (see [fidelity](#fidelity))  |

## Expected coverage

| Work done                    | What renders                                                                                       |
|------------------------------|----------------------------------------------------------------------------------------------------|
| A1 + A2                      | diagrams made of basic shapes, round strokes, charts (bars, lines, pies with `od_draw_sector`), flat badges |
| + A3 + A4                    | every path fill and stroke exactly, any fill rule, any cap / join / dash: icon sets, logos, exported charts, detailed illustrations, text-as-paths charts, simplified maps |
| never                        | gradients (solid fallback), illustrations relying on blur / masks / layer opacity, pattern-filled maps, text-heavy SVGs with custom fonts (rendered with the fallback families, layout off) |

## Order

1. CL0 (optional): fix the stale `od_set_canvas` header comment (it says text sizes/advances are not scaled by the
   canvas; the code scales them — `select_face`: `physical = size * canvas.scale`, consistent with the
   `od_draw_text` / `od_text_width` docs). Comment-only, no version bump.
2. CL1: A0 + A1 + A2 — `extra/svg.c/.h`, `third_party/sv.h`, the `svg` test target (CMake + `tests/svg_test.c`), the
   named compare function in `compare.c`, the `tests/svg/` fixtures and the first `tests/references/svg.png`. No
   version bump.
3. CL2: A3 — the untangler, cuts fills into pieces for `od_draw_polygon`, 32 px cells; the CPU untangle corpus, the
   piece histogram. No version bump.
4. CL3: A4 — the stroker on top of the untangler, the dash walker. No version bump.
5. CL4: A5 — the small items and the unsupported-content flags. No version bump.
6. Later, each its own CL: `<image>` (stb_image -> atlas), the full-screen canvas shortcut, embedded SVG, the A3 cell
   size retune (after the GPU profiling run), B2 when a scene needs it (3.8.0).
