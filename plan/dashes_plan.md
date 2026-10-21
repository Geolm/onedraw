# Plan: dashed shapes

## Goal
Draw dashed lines, rings, ellipse rings, triangle rings and Béziers without making the rasterizer heavier. Dashes are cut
on the CPU into ordinary primitives (capsules, or arcs for circles), so the shaders do not change.

## Design decisions
- **Separate functions, not a global dash state.** Drawing a dashed circle is a different process from drawing a circle
  (hundreds of commands plus a group instead of one command). `od_draw_ring` and friends stay untouched.
- **Style struct passed by value**, so each function keeps a short parameter list:

```c
typedef struct od_dash_style
{
    float dash_length;   // along the path, in pixels
    float gap_length;
    float offset;        // phase, e.g. for marching ants
    bool  round_caps;    // capsules (true) or flat ends (false)
} od_dash_style;

uint32_t od_draw_dashed_line(r, ax, ay, bx, by, width, od_dash_style, color);
uint32_t od_draw_dashed_ring(r, cx, cy, radius, thickness, od_dash_style, color);
uint32_t od_draw_dashed_ellipse_ring(r, ax, ay, bx, by, width, thickness, od_dash_style, color);
uint32_t od_draw_dashed_triangle_ring(r, const float* vertices, thickness, od_dash_style, color);
uint32_t od_draw_dashed_quadratic_bezier(r, const float* control_points, width, od_dash_style, color);
uint32_t od_draw_dashed_cubic_bezier(r, const float* control_points, width, od_dash_style, color);
```

Return value follows the existing Bézier convention: number of commands emitted, `UINT32_MAX` on failure (tessellation
stack overflow or command budget exhausted).

## Implementation
All in `lib/onedraw.cpp`, next to the Bézier code, no shader change.

1. **Shared dasher** (static): `emit_dashes(path walker, style, closed)`.
   - Walks a polyline by arc length and emits one capsule (`od_draw_capsule`) per dash.
   - **Phase carry:** the remaining dash/gap length carries across polyline segments, so a dash spanning a corner becomes
     two capsules sharing the corner point (round caps give a round join for free).
   - **Closed shapes:** pick `n = round(perimeter / (dash + gap))` and rescale the period to `perimeter / n`, so the
     pattern closes without a stub at the seam. `offset` is applied after the rescale.
   - **Translucent colors:** wrap the output in one `od_begin_group` / `od_end_group` so overlapping capsule ends do not
     double-blend. Before doing so, check how `od_begin_group` behaves when already inside a group (nesting). If nesting
     is unsupported, skip the wrap or fail loudly when the caller is already grouping.
   - **Budget:** one command per dash against `MAX_COMMANDS` (65536; `tile_node.command_index` is 16 bits). Check the
     remaining capacity up front (estimated from path length / period) and return `UINT32_MAX` instead of emitting a partial
     shape.
2. **Flatteners**, one per shape, feeding the walker:
   - line: the segment itself.
   - Bézier: reuse the existing De Casteljau tessellation, but collect the points instead of drawing capsules.
   - triangle ring: the three edges in order, closed.
   - circle ring: analytic, stepping the angle (see arc vs capsule below).
   - ellipse ring: sample `center + R * (a*cos t, b*sin t)` with a step count driven by curvature, accumulate lengths,
     then walk. Needed because an ellipse has no closed-form arc-length parametrization.
3. **Circle: capsules vs arcs.** The ring has two modes selected by `round_caps`:
   - `round_caps = true`: capsules along the circle. Chord error is negligible at dash scale.
   - `round_caps = false`: `n` arcs of constant sweep via the existing arc primitive (exact, but flat ends).
   Only `od_draw_dashed_ring` needs to know; plain `od_draw_ring` is unaffected.

## Known risk: arc culling with small apertures
Dashed arcs on large circles have very small apertures (for example radius 500 and a 5 px dash is about 0.01 rad). I
suspect the tile test `intersection_aabb_pie` (`src/shaders/binning.metal`) can reject tiles that the arc actually
touches. This has not been tested.

How the test works: it first checks the tile against the full disc, then tests the 4 tile corners against the cone
(`dot(normalize(corner - center), direction) > aperture.y`), and if none is inside it falls back to a ray test
(`intersection_aabb_ray`) along the *central* direction only. A thin cone can clip a tile edge with no tile corner inside
the cone while the central ray misses the tile, so the tile is dropped and the dash disappears (or loses a chunk) on that
tile. With a 16 px tile much wider than the cone at that radius, this is likely, not an edge case.

Related, smaller issue: the cone test ignores the AA margin and half-thickness at the flat ends, so the ~1 px AA fringe at
the end of a dash can be cut on tiles that only touch it.

Mitigations, in order of preference:
1. Use capsules (`round_caps = true`, and also as the default for dashes). Capsule culling is the OBB test
   (`intersection_aabb_obb`), which is exact for the capsule's box. Arcs are then only used if the test below shows they are
   fine.
2. If arcs are wanted for exact flat ends, fix `intersection_aabb_pie`/`intersection_aabb_arc`: also test the two cone edge
   rays (or do a proper box-vs-wedge test with the edge directions), and widen the cone slightly by the AA margin.
3. Or dash circles with flat ends using oriented boxes tangent to the circle (OBB culling, no arc primitive); the small
   geometric error is fixed by using short dashes.

### Test to run before relying on arcs
- Draw a dashed circle with radius 50/200/500/1000, dash 3/5/12 px, thickness 2/6 px, using arcs, with `od_set_culling_debug`
  enabled. Look for missing dashes or gaps in dashes at tile boundaries.
- Repeat with `culling_debug` off and compare against the capsule version of the same circle (screenshot diff, or
  per-pixel diff with `od_take_screenshot`).
- Also test circle positions that sweep the dash across tile corners (animate the center by sub-pixel steps).

## Verification
- Visual: each shape with dash/gap/offset variations, both cap modes, opaque and translucent colors (no darker joints).
- Closed shapes: no stub at the seam for several radii/perimeters.
- Budget: a very long dashed Bézier returns `UINT32_MAX` and leaves the command list unchanged.
- Animate `offset` to check phase continuity across corners.
- Command count and GPU time (`od_get_stats`, `od_get_average_gputime`) for a large dashed circle versus a solid ring.

## Out of scope
Per-segment variable width, dash patterns with more than one dash/gap pair, caps other than round/flat, shader changes,
and the unrelated `clip_pixel` precedence bug in `rasterizer.metal` (tracked separately).
