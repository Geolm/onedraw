# onedraw — a GPU-driven 2D renderer

## Binning commands

Binning is the step that generates the **per-tile linked lists** of draw commands. It works by testing each draw command against the bounding box of every tile, using a classic intersection method per shape type:

* **oriented boxes / lines / capsules** → Separating Axis Theorem (SAT)
* **discs** → distance-to-nearest-point (plus a "ring" test for hollow ones)
* **pies / arcs** → disc test + a ray / angular slab test
* **triangles** → SAT on the three edges, plus an all-vertices-inside shortcut
* **ellipses** → the tile is mapped into the ellipse's local space, then treated as a circle

Three extra factors are folded into the same test, and each of them makes the effective bounds of a command *bigger* than its mathematical bounds:

* [anti-aliasing](#anti-aliasing)
* [groups of shapes](#group-of-shapes)
* [smoothmin](#smoothmin)

The section below explains how each one is handled so that nothing that touches a tile is missed.

---

## Anti-aliasing

The width of the anti-aliasing region must be accounted for during the intersection test.

![Growing the test box for anti-aliasing](anti-aliasing.png)

In the example above, the disc does not mathematically intersect the tile's bounding box — but its anti-aliasing band does. If we skipped it, we would get visible seams or hard edges along the tile border. So the tile's bounding box is simply **grown by the anti-aliasing width** before the test, and the disc is kept in the tile's list so its edge pixels are shaded correctly.

---

## Group of shapes

`onedraw` lets you combine several shapes into a **group** that behaves like a single shape: it can have a global transparency even though it contains multiple layers, or an outline that applies to the whole group.

A group is defined by wrapping shapes between **begin group** and **end group** commands; the end group command is where the color output (and, if enabled, the outline) is produced.

![Groups of shapes](group.png)

Each begin/end group command carries a **global bounding box** that encompasses every shape in the group, so both commands are included whenever the group's box touches a tile. If a group has no shapes reaching a given tile, its begin/end pair is removed from that tile's list to avoid pointless work:

![Empty groups are cleaned per tile](group-empty.png)

On the CPU side, `od_begin_group()` reserves one slot in the quantized-AABB buffer, every shape drawn while the group is open is merged into it, and `od_end_group()` copies the accumulated box onto the end command. On the GPU, the `tile_bin` kernel walks the (reverse-order) list while tracking the current group's extra margin. It defers a group's `end` command and only emits the `begin`/`end` pair once a primitive in the group actually reaches the tile, so a group that wraps zero primitives for that tile produces no nodes — no separate clean-up pass.

---

## Smoothmin

The **smoothmin** operator lets several shapes blend smoothly even when they do not intersect. The *k-factor* (from [Inigo Quilez's website](https://iquilezles.org/articles/smin/)) controls how far a shape can blend with another.

![smoothmin blending](smoothmin.png)

During binning, the tile's bounding box is **grown by this k value** so that a shape that contributes to a smooth blend is never missed.

> **Note** — this is a deliberately conservative way to handle smoothmin. A tighter solution would use [interval arithmetic](https://www.mattkeeter.com/research/mpr/), but a few considerations weigh against it:
>
> * we do not support complex trees of boolean operations — only `smoothmin` would benefit from interval arithmetic (`min` is already optimal);
> * the binning shader is already VGPR-bound, so adding interval evaluation for every primitive kind would cost performance even in frames that never use smoothmin;
> * a promising direction is a **hybrid**: evaluate intervals on the CPU and let them adjust the AABB that is tested on the GPU.

---

## Tile cache

The tile cache skips the fragment work of the tiles that did not change between frames. It is **opt-in** through the `cache` flag of `onedraw_def`: with the flag off, the pipeline does exactly what it did before. The whole test runs on the GPU, with no readback:

* a `hash_commands` kernel hashes the *content* of every draw command (its type, color, data floats, clip, quantized bounds) into one `uint` per command — the command's *position* in the list is deliberately not part of the hash, so re-ordering or inserting commands elsewhere does not invalidate a tile;
* in `tile_bin`, every command folded into a tile also folds its content hash into the tile's hash, which is seeded from a **frame seed** mixing everything that can change a pixel without changing the commands (clear color, anti-aliasing width, atlas generation, font generation, ...);
* the new tile hash is compared against the previous frame's in a persistent buffer: a match makes the tile **clean**, a mismatch makes it **dirty**. A freshly allocated hash buffer is filled with a sentinel value that can never match a real hash, so the first frame always redraws.

At rasterization time the two categories take two paths. A **dirty** tile is shaded exactly as before, and its final color is written into a persistent cache texture in the **backbuffer format** (`BGRA8Unorm`, or the `_sRGB` variant when the backbuffer is). A **clean** tile is redrawn by a *second* indirect draw that reuses the tile vertex shader and a fragment shader that only reads the tile's own texel back from the cache — the whole command walk is skipped. Because the dirty path stores the very color the tile would produce and the clean path returns the stored texel unchanged, the output is bit-exact whether the cache is on or off. Blending must remain off for this to hold (it is, by design). For an sRGB backbuffer the store converts and the read decodes, and the ROP re-encodes on the way out — a round-trip that is exact by construction, though the sRGB path is not exercised by the tests yet.

The price is `4 bytes` per pixel for the cache texture (~14 MiB at `2560 × 1440`, up to 64 MiB at the `4096 × 4096` maximum), plus `4 bytes` per tile for the stored hash and `4 bytes` per command for the content hash. Any atlas upload, font scale change, or resize bumps the frame seed and forces a full redraw.

### Measured gain

On the [test scene](../tests/test.c) — mostly static, with a single moving arrow — at `2560 × 1440` on an M5 Pro (20 GPU cores), the cache cuts the GPU frame time from **1.69 ms** to **1.11 ms** (−34 %). The gain scales with how much of the screen stays static: a fully static scene approaches the cost of a full-screen texture copy, and a fully moving scene pays the extra cache write on every tile.

> **Note** — in *culling-debug* mode, dirty tiles are tinted red and clean (cached) tiles green, so cache hits and misses are visible while moving the scene.

---

[Next part](part2.md) : Hierarchical binning and intrinsic tricks
