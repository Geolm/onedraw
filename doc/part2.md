# onedraw — a GPU-driven 2D renderer

## Performance considerations

Even though binning runs on the GPU, it is not free. Let's look at the raw numbers:

* At 1440p the screen holds **160 × 90 = 14,400 tiles**
* `onedraw` supports up to **65,536 draw commands**
* In the worst case that is roughly **943 million** intersection tests

A naïve command×tile pass could easily cost more than the actual rasterization. The two ideas below — quantized AABBs and hierarchical binning — keep the real work bounded.

---

## Quantized AABB

The first optimization is to store an **AABB** (axis-aligned bounding box) per command and use it as a cheap pre-test before any heavier intersection math. The boxes are **quantized** to the tile resolution (16 px): each edge is a single byte holding a *tile* index (`0…255`), which also caps the viewport at 256 × 16 = 4096 px.

The AABBs are also kept in a **separate buffer** from the draw commands, avoiding cache thrash and giving the region bin pass a tight, coalesced read.

---

## Hierarchical binning

Even with AABB pre-tests, many commands still force each tile to do a lot of work. To reduce that, we introduce **hierarchical binning** — pre-building a list of commands for larger screen regions:

![Hierarchical binning](hierarchy.png)

A region covers **16 × 16 tiles**, and each region keeps a list of the commands that affect it. When binning individual tiles, we only consider the commands from that region's list instead of all global commands.

The catch: one thread per *region* would be too few threads — there are few regions but many commands, and GPUs run best with a **large number of lightweight threads**, not a few heavy ones. `onedraw` splits the difference: the region's list is built by **one threadgroup of up to 1,024 threads per region**, which walks the command list and keeps it in **command ordering** via a per-step prefix sum — the next section.

## Fused region bin

Earlier designs built each region's list with three separate kernels — a per-command **predicate**, an **exclusive scan**, and a **scatter** — going through two region-sized buffers. `onedraw` fuses all three into a **single kernel** (`region_bin`). One **threadgroup per region** (up to 1,024 threads, 32 waves) walks the command list in steps of its size; each thread tests one command per step, reading neighbour AABBs in a coalesced pattern so the (shared) AABB buffer stays in cache. The visibility is recomputed each step rather than stored, so there is no predicate or scan buffer to allocate.

![The fused region bin: one pass builds one ordered list](onepass-scan.png)

Each step runs the same three-stage compacting that the separate-kernel design needed — a test, a prefix sum, and a scatter:

![Test, scan, scatter — in draw order](predicate.png)

* a **test** of the command's AABB against the region gives a `0` / `1` visibility,
* an **exclusive scan** (the prefix sum below) turns the `1`s into a compact slot,
* a **scatter** writes the visible commands into the region's list, in order.

With real values for one region, a visible command's scan value is exactly its slot in the compact list, and hidden commands fall through:

![One region, with real values](scan-example.png)

The prefix sum runs in one dispatch per region, collapsed by two facts:

1. **The scan is per region.** Each threadgroup is one region's line of threads. The list is built in steps of the threadgroup size, so each step's prefix fits in a dispatch of at most **1,024 threads** (a multiple of the 32-lane wavefront, which `simd_prefix_exclusive_sum` requires).
2. **SIMD intrinsics collapse the middle level.** Each 32-lane wave reduces with `simd_sum` / `simd_prefix_exclusive_sum`, which operate on registers — no global-memory round trip and no extra kernel.

Inside each step the prefix sum runs four levels:

1. **Lane.** `simd_prefix_exclusive_sum` over the 32 lanes of a wave gives each command its in-wave offset.
2. **Wave.** `simd_sum` hands every lane the wave's visible count; the first lane stores it to threadgroup memory. → **barrier**
3. **Threadgroup.** The first `num_waves` lanes run `simd_prefix_exclusive_sum` over the per-wave totals to compute each wave's offset. → **barrier**
4. **Combine.** Each visible command is scattered at `running_total + wave_offset + lane_offset`, where `running_total` carries the count from the previous step.

The only shared-memory traffic is the ≤ 32 per-wave totals. The `region_bin` writes indices in a **reversed** order (top-most first) and ends each list with a `LAST_COMMAND` terminator, so the output lists are byte-identical to the older three-kernel chain — but the predicate and scan buffers, and two kernel launches, are gone.

You can look at the [binning shader](../src/shaders/binning.metal).

---

## Occlusion culling

The region's list is kept in draw order, and the `tile_bin` walk goes from the top-most command down — so as soon as it meets a command that **completely hides the tile**, everything below it can be dropped: those commands cannot contribute a single pixel, and the walk stops.

Only **axis-aligned boxes** can occlude, and only when all of these hold — the test is deliberately conservative, a false negative just costs a little extra rasterization:

* **solid** fill — a blurred or dashed box is not a solid cover;
* **opaque** (alpha 255) — and not inside a **group**, whose global color and outline are composited at the end and could still lighten it;
* **not clipped** — a clip could cut part of the cover away;
* **strictly containing the tile** — tested against the box's own parameters shrunk inward by its **corner radius** (a rounded box is not opaque inside its corners) plus a `0.01` px epsilon, rather than against the quantized AABB, which is grown by the anti-aliasing margin and would occlude tiles the box only grazes.

Other shape kinds (discs, triangles, ...) could be tested the same way, but a full 16×16 tile cover happens almost exclusively with boxes in practice, so the extra math is not worth it.

The output is bit-exact either way — a hidden command would have blended nothing on top of an opaque cover. The win scales with how much content sits under opaque shapes, which is the common UI case: a window over a busy background.

---

[Next part](part3.md) : Rasterization
