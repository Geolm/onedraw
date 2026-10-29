# Performance and memory plan

## Goal

Reduce GPU memory and frame time with the **same feature set** and no public API change.
Every step is measured before/after (AGENTS.md: no unmeasured optimization) and must pass `./build/test --autoexit --compare`.

Figures below are for 2560x1440 (160x90 tiles, 10x6 = 60 regions) unless noted, 4K is 3840x2160 (135 regions).

| Consumer (1440p)        | Size      | Notes                                                    |
|-------------------------|-----------|----------------------------------------------------------|
| `tiles.nodes`           | 32 MiB    | fixed, `MAX_NODES_COUNT` x 8 bytes                       |
| `regions.indices`       | 7.5 MiB   | `regions.count * MAX_COMMANDS * 2`                       |
| `regions.scan`          | 7.5 MiB   | `regions.count * MAX_COMMANDS * 2`                       |
| `regions.predicate`     | 3.75 MiB  | `regions.count * MAX_COMMANDS * 1`                       |
| screenshot texture      | 14 MiB    | only with `allow_screenshot`                             |
| CPU ring (x3)           | 6 MiB     | commands, colors, data, aabb, clips ; step 6 goes to x2  |

## Step 0 : measurement first

The fine GPU timings (`binning_time_ms`, `rasterization_time_ms`) are implemented (commit d709c04) and `./build/test --autoexit` prints them at exit ;
`--validate` enables the Metal validation layer.

Baseline, MacBook Pro 2x (1280x720 window -> 2560x1440 framebuffer, 160x90 tiles, 10x6 = 60 regions), averaged over the last 40 frames, scene ~790 commands :

| cache | gpu | binning | rasterization | gpu memory |
|-------|-----|---------|---------------|------------|
| off   | 1.91 ms | 1.04 ms | 0.80 ms | 74661 kb (72.9 MiB) |
| on    | 1.72 ms | 0.94 ms | 0.72 ms | 89637 kb (87.5 MiB) |

The few "out of draw commands" lines are the step 1 regression frame (pads one command over the ring on
frame 10, outside the averaged window). Recorded on the step 1 tree : the cap and the stats null check add
no buffer and no work to the averaged frames, so the numbers equal the pre-fix baseline.

## Step 1 : bug fixes

| Bug | Location | Fix |
|-----|----------|-----|
| Command index 65535 equals `LAST_COMMAND` and is read as the region list terminator, the 65536th command is dropped (it is list position 0, so the whole region is dropped) ; the `uint16` scan totals overflow when 65536 commands are visible | `binning.metal:731`, `exclusive_scan` | size the command rings on `LAST_COMMAND` (latest index 65534) so `LAST_COMMAND` stays a free terminator ; the scan totals are then at most 65535 and fit in `uint16` |
| `od_get_stats` dereferences `r->rasterizer.atlas` when no atlas was created | `onedraw.cpp:1362` | null check like the screenshot texture |

## Step 2 : remove the per-frame `regions.indices` clear

- Today `od_bin_commands` fills the whole `regions.indices` buffer with `0xff` every frame (`onedraw.cpp:728`) only so `tile_bin` finds a terminator : 7.5 MiB/frame at 1440p, 17 MiB at 4K.
- The scan knows each region's visible count : thread 0 of `exclusive_scan` sums the simdgroup totals already in threadgroup memory and writes `LAST_COMMAND` at `region_offset + count` when `count < num_commands` (the `for i < num_commands` bound already covers the full case). The host binds `regions.indices` to the `exclusive_scan` dispatch and drops the blit fill.
- After step 4 (head kept in a register and always written), the `tiles.head` clear can go too.
- Measured, same machine and scene, reference identical : the fill sits in the blit encoder, outside the binning sample, so the mechanical win is on the gpu total, no memory change.

| cache | gpu | binning | rasterization | gpu memory |
|-------|-----|---------|---------------|------------|
| off   | 1.19 ms | 0.36 ms | 0.60 ms | 74661 kb |
| on    | 0.60 ms | 0.36 ms | 0.20 ms | 89637 kb |

(before : 1.91/1.04/0.80 and 1.72/0.94/0.72 ms, same memory). The measured binning drop (1.04 -> 0.36) exceeds what removing the fill explains on its own : this machine shows large run to run variance, treat the absolute numbers with care, the direction is solid.

## Step 3 : fuse predicate / exclusive scan / region bin into one kernel

### Why

Today the region lists are built by three kernels that go through two region-sized buffers :

| Kernel | Reads | Writes |
|--------|-------|--------|
| `predicate` (1 thread per command, loops over the regions) | aabb | `predicate[region][cmd]`, 1 byte |
| `exclusive_scan` (1 threadgroup per region, each thread a contiguous chunk of `num_elements_per_thread`) | `predicate` | `scan[region][cmd]`, 2 bytes, written then read-modify-written |
| `region_bin` (1 thread per region x command) | `predicate`, `scan` | `regions_indices`, visible commands only |

That is 9 bytes of device memory traffic per (region, command) (predicate write 1, scan 1 + 2 + 2 + 2, region_bin predicate read 1 ; 11 with the
step 2 clear), plus 4 per visible command (scan read, index write), and 3 bytes of storage per (region, `MAX_COMMANDS`). The visibility itself is a 4-byte aabb test (`binning.metal:529`) : it is cheaper to recompute than to store.
The scan also reads with a stride of `num_elements_per_thread` between neighbour threads, so its reads are not coalesced.

### New kernel

A single kernel (keeps the `region_bin` name and PSO) : one threadgroup per region, up to 1024 threads (32 simd groups). The threadgroup walks the
command list in steps of its size, each thread tests one command per step : neighbour threads read neighbour aabbs (coalesced), and every region
threadgroup streams the same aabb buffer, which stays in cache.

```metal
kernel void region_bin(constant draw_cmd_arguments& input [[buffer(0)]],
                       device uint16_t* regions_indices [[buffer(1)]],
                       threadgroup uint32_t* simd_offsets [[threadgroup(0)]],          // SIMD_GROUP_SIZE + 1 entries
                       uint thread_index [[thread_index_in_threadgroup]],
                       uint2 tg_size [[threads_per_threadgroup]],
                       uint simd_group_id [[simdgroup_index_in_threadgroup]],
                       uint simd_lane [[thread_index_in_simdgroup]],
                       uint num_simd_groups [[simdgroups_per_threadgroup]],
                       uint2 group_position [[threadgroup_position_in_grid]])
{
    uint region_index = group_position.y;
    uint2 region_xy = uint2(region_index % input.num_region_width, region_index / input.num_region_width);
    uint region_offset = region_index * input.num_commands;
    uint running_total = 0;

    for (uint base = 0; base < input.num_commands; base += tg_size.x)
    {
        uint index = base + thread_index;
        uint cmd_index = input.num_commands - index - 1;    // reverse order for the tile linked list
        bool visible = false;
        if (index < input.num_commands)
        {
            quantized_aabb aabb = input.commands_aabb[cmd_index];
            visible = all(region_xy >= uint2(aabb.min_x, aabb.min_y) / REGION_SIZE) &&
                      all(region_xy <= uint2(aabb.max_x, aabb.max_y) / REGION_SIZE);
        }

        uint lane_offset = simd_prefix_exclusive_sum((uint) visible);
        uint simd_count = simd_sum((uint) visible);
        if (simd_is_first())
            simd_offsets[simd_group_id] = simd_count;
        threadgroup_barrier(mem_flags::mem_threadgroup);

        if (simd_group_id == 0)
        {
            uint count = (simd_lane < num_simd_groups) ? simd_offsets[simd_lane] : 0;
            uint total = simd_sum(count);
            simd_offsets[simd_lane] = simd_prefix_exclusive_sum(count);
            if (simd_lane == 0)
                simd_offsets[SIMD_GROUP_SIZE] = total;
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);

        if (visible)
            regions_indices[region_offset + running_total + simd_offsets[simd_group_id] + lane_offset] = (uint16_t) cmd_index;

        running_total += simd_offsets[SIMD_GROUP_SIZE];
        threadgroup_barrier(mem_flags::mem_threadgroup);   // simd_offsets is rewritten by the next step
    }

    if (thread_index == 0 && running_total < input.num_commands)
        regions_indices[region_offset + running_total] = LAST_COMMAND;    // the terminator chosen in step 1
}
```

- Traffic : the 4-byte aabb read per command (shared by all regions through the cache) and 2 bytes per visible command, nothing else.
- Order : the prefix sums keep the order inside a step and `running_total` carries it across steps, so the lists are identical to today's
  (same reverse order, top-most first, as `tile_bin` expects). The output images must not change.
- `running_total` is a `uint32_t` : no `uint16_t` total overflow at 65536 visible commands (covers that part of step 1).
- The terminator write replaces the step 2 clear of `regions.indices` ; when every command is visible the `num_commands` bound of `tile_bin` ends the walk.
- The calls to `simd_prefix_exclusive_sum` / `simd_sum` are outside any divergent branch, the thread count stays a multiple of `SIMD_GROUP_SIZE`.

### Host side

- `od_bin_commands` : the three dispatches become one, same shape as today's scan :
  `threads_per_region = optimal_num_threads(count, SIMD_GROUP_SIZE, MAX_THREADS_PER_THREADGROUP)`,
  `setThreadgroupMemoryLength((SIMD_GROUP_SIZE + 1) * sizeof(uint32_t), 0)`,
  `dispatchThreads(MTL::Size(threads_per_region, regions.count, 1), MTL::Size(threads_per_region, 1, 1))`, buffers 0 (draw args) and 1 (`regions.indices`).
- Delete `regions.predicate_pso`, `regions.exclusive_scan_pso`, `regions.predicate`, `regions.scan` and their create / release / stats lines
  (`od_build_pso`, the null check of `od_bin_commands`, `od_resize`, `od_terminate`, `od_get_stats`).
- Delete `draw_cmd_arguments.num_elements_per_thread`, and `num_groups` which no shader reads (`common.h`, internal to the cpp and the shaders, not public API).
- `lib/binning.h` is regenerated by the pre-build step (non Xcode generators) and committed with the change.

### Cost and risks

- Loop steps : `ceil(count / 1024)`, 64 at 65536 commands, 3 barriers each. Today's scan has 2 barriers in total but uncoalesced reads.
  If the barriers dominate : double-buffer `simd_offsets` (alternate two arrays by step parity) to drop the last barrier, or let each thread test
  2 to 4 consecutive commands per step.
- Parallelism is `regions.count` threadgroups (60 at 1440p, 135 at 4K) : enough to fill the GPU. Small viewports have few regions but little work.
  Splitting a region over several threadgroups needs a cross-threadgroup scan, only worth it if the timings show this kernel is the bottleneck.

### Expected and validation

- Memory : -11.25 MiB at 1440p, -25 MiB at 4K (`gpu_memory_usage`).
- `binning_time_ms` down ; reference images unchanged (`--compare`), `--validate` clean.
- Regression test : a frame with more than 1024 commands spread over several regions (exercises the carry between steps), and a count that is not a
  multiple of 32.

## Step 4 : `tile_bin` traffic

1. Keep the tile head in a register, write `output.head[tile_index]` once at the end (today one device read + write per node, `binning.metal:786-790`).
2. Drop `clean_list` (`binning.metal:628`, a second walk of the list in device memory) : a group can be empty in a given tile (its merged aabb
   touches the tile, none of its shapes do).
   In the main loop (reverse order, `end_group` comes first) : don't add the `end_group` node when it is reached, keep its command index as pending ;
   on the first primitive added to the tile inside the group, add the pending `end_group` node first ; on `begin_group`, add it only if the
   `end_group` was added. No node is removed and none is wasted ; groups don't nest, so a single pending index is enough.

After 4.2, `tile_node.command_type` has no reader (the fragment shader reads the type from the command).

## Step 5 : occlusion culling in `tile_bin`

- `tile_bin` walks the commands top-most first. When an added command fully covers the tile with alpha 1, add it and `break` : everything below is
  hidden in this tile.
- The binning pass is already ~40% of the frame : the check is limited to a few cheap shapes, and ordered so almost every command leaves at the
  first (integer) test :
  1. type is `primitive_aabox`, `primitive_disc`, `primitive_triangle`, or `primitive_oriented_box` with `width == 0` (capsule)
  2. `cmd_aabb.max_x > tile_xy.x && cmd_aabb.max_y > tile_xy.y`
  3. `fillmode == fill_solid`
  4. color alpha 255
  5. `clip_index == 0`
  6. not inside a group (between an `end_group` and its `begin_group` in the reverse walk) ; groups are not handled
  7. the containment test below
- Test 2 is an exact necessary condition, independent of the aabb margin : covering tile `k` means reaching its far edge `16(k+1)`, so the quantized
  max is at least `k+1`. It rejects every single-tile command (the ones accepted without an intersection test) and the last column / row of tiles of
  any command, with two integer compares on values already loaded. The min side is not tested : the clamp to 0 at the screen edge makes it a special case.
- No `aa_width` margin is needed : `tile_fs` gives alpha 1 wherever `distance <= 0`, the anti-aliased fringe is outside the shape. Testing the tile
  corners (not the pixel centers) already leaves half a pixel ; use a small epsilon (inside by 0.01 px) so binning and the fragment SDF agree.
  All four shapes are convex : the four tile corners inside means the whole tile is inside.

| Shape | Containment test | Cost |
|-------|------------------|------|
| box | tile inside the box shrunk by its corner radius on both axes (exact when the radius is 0, conservative otherwise) | 4 compares |
| disc | farthest tile corner `max(abs(center - tile.min), abs(center - tile.max))` within the radius ; only possible from a ~11.3 px radius (tile half-diagonal) | 1 squared length |
| capsule | the four tile corners within the radius of the segment (`data[5]`) | 4 point-segment distances |
| triangle | `is_aabb_inside_triangle` (already used for hollow triangles), against the sharp triangle : conservative with a roundness | 4 x 3 edge functions |

- Not covered on purpose : groups, ellipse, pie, arc, oriented box with a width, quads, chars, blurred box, hollow and gradient fills.
- Gains : fewer nodes, fewer SDF evaluations per pixel, shorter `tile_bin` loops (the `break` skips the rest of the region list for that tile), more
  cache hits (changes under an opaque panel no longer dirty the tile).
- Measure on the test scene and on a UI-like scene (stacked opaque panels) ; keep the step only if the frame time drops.
- Risk : `mix(prev, c, 1.0h)` is not guaranteed to round to exactly `c`, a few pixels can move by one 8-bit step ; check the PSNR, regenerate the
  references only if the change is intentional and visually identical.
- Test : for each of the four shapes, an opaque one covering other shapes renders the same as before ; plus the cases that must not cull
  (translucent color, clip, inside a group, partial coverage).

## Step 6 : two frames in flight

`od_flush` ends with `waitUntilCompleted()` (`onedraw.cpp:997`, present since the first commit) : the CPU waits for the GPU every frame, so frames never
overlap and 2 of the 3 ring copies are unused. Decision : keep 2 frames in flight, like most renderers, instead of making the renderer fully synchronous.

| Frames in flight | CPU ring | CPU/GPU overlap |
|------------------|----------|-----------------|
| 1 (today, in practice) | 2 MiB used, 6 MiB allocated | none |
| **2 (chosen)** | 4 MiB | yes |
| 3 | 6 MiB | yes, more slack |

Changes :

1. Move `dispatch_semaphore_wait` from `od_flush` (`:880`) to `od_begin_frame`, before the ring buffers are mapped (`:1262`). Today the ring is written
   before the wait, harmless only because of the end of frame wait.
2. Remove `waitUntilCompleted()` from `od_flush`, except when the frame takes a screenshot : `out_pixels` stays filled when `od_end_frame` returns.
3. Keep the last committed command buffer (retained) ; `od_resize` and `od_terminate` call `waitUntilCompleted()` on it before releasing resources.
   The queue executes in order, so the last one completing means all previous ones did.
4. `DynamicBuffer::MaxInflightBuffers` = 2 (also used for the semaphore count).
5. GPU timing : the single `sample_buffer` relies on the end of frame wait (`onedraw.cpp:1085`). With 2 frames in flight, frame N+1 can overwrite the
   samples before frame N's completion handler resolves them : use one sample range per frame in flight (a buffer of `4 * MaxInflightBuffers` samples,
   indexed by `frame_index % MaxInflightBuffers`, resolved by the handler of that frame).

The GPU-only buffers (`nodes`, `regions.*`, `tiles.*`, cache texture, tile hashes) stay single : Metal's hazard tracking orders frame N+1's clears after
frame N's reads. Confirm that this also covers the resources reached through argument buffers and declared with `useResource` : run
`./build/test --autoexit --validate` and check that a moving scene renders the same as with the wait.

Threading : the completion handler runs on another thread ; `gpu_time` is already atomic and `r->screenshot` is only touched on frames the CPU waits on.

Validation : CPU time (`od_begin_frame` to `commit`) and frame time before/after ; expected frame time ~ max(CPU, GPU) instead of CPU + GPU, -2 MiB.

## Step 7 : node pool layout (largest change)

- Replace one 8-byte node per command by chunks, e.g. 16 bytes = `next` + six `uint16` command indices (~2.7 bytes per command instead of 8).
- `tile_bin` fills the current chunk in registers, from the last slot down (reverse traversal), and prepends it when full ; unused slots hold a sentinel skipped by the fragment shader.
- About 3x less node memory for the same capacity (or the same capacity in ~11 MiB), and ~6x fewer dependent loads in `tile_fs`.
- Size the pool from the peak measured in step 0 ; do it after step 4 and 5 since both change the node count.

## Step 8 : minor

- Screenshot : copy only the capture region instead of the whole drawable (`onedraw.cpp:991`).
- Batch the per-frame `useResource` calls (`useResources` or a heap), only if a CPU profile shows them.

## Step 9 : tile cache, skip the clean regions (after step 3)

Today `tile_bin` builds the full list of every tile (intersection tests, node writes, atomics, `binning.metal:728-793`) and only then compares the
tile hash (`:799-821`) : a clean tile is drawn by `clean_fs` from the cache texture, its list is never read. On a mostly static frame almost all
the `tile_bin` work is wasted.

1. Region hash in the fused kernel of step 3 (only when `input.cache`) : the region list is exactly the set of commands that can touch a tile of
   the region. Each visible thread mixes `command_hashes[cmd_index]` with its position in the compacted list, the threadgroup combines them with
   an order-independent reduction (xor or sum, `simd_sum` then the threadgroup totals already used by the scan) ; the position keeps the order in the hash.
   At the end, mix `frame_seed` and the count, compare with the stored region hash and write the new one.
2. New buffer `regions.hashes` (one `uint32_t` per region, 240 bytes at 1440p), filled with the sentinel at resize like `tiles.hashes` ; a
   sentinel stored value is always dirty. The clean flag goes to `tile_bin` (one byte per region, or the hash comparison redone from the two buffers).
3. `tile_bin` early out when the region is clean : every tile has the same list as last frame, its stored tile hash is still valid.
   - stored tile hash == `frame_seed` : the tile was empty (an empty tile keeps the initial `tile_hash = frame_seed`), nothing to do ;
   - otherwise : push it to `clean_indices` and return. No walk, no node, no intersection test.
   - the branch is uniform per threadgroup (`thread_pos.z` is the region).
4. A region that changed even partially takes today's per-tile path : the result is exact, only the work changes.

- Cost : one hash and a reduction per visible command per region in the fused kernel, small next to `tile_bin`.
- Gain : `tile_bin` is almost free in unchanged regions (static UI, a blinking cursor dirties one region). No memory gain : the node pool is still
  sized for a fully dirty frame.
- Measure on a static scene and a partly animated one (a fully animated test scene shows only the cost) ; `--compare` must not change.
- Test : a scene animated in one region only renders the same as with the cache off, over several frames ; a region going from content to empty
  and back.
- Fallback if regions are too coarse (measure first) : a tile-level pre-pass hashing the commands whose quantized aabb touches the tile (a
  superset of the added ones, so still exact), full walk only when dirty. Dirty tiles pay an extra loop, and a dirty tile with an empty list is
  not drawn : its stored hash must be the sentinel so it is never seen as clean on stale cache content.

## Order and validation

| Step | Risk | Memory | Time |
|------|------|--------|------|
| 0 baseline (timings done) | low | - | - |
| 1 bug fixes | low | - | - |
| 2 no indices clear | low | - | binning |
| 4 tile_bin traffic | low | - | binning |
| 3 fused scan | medium | -11 MiB | binning |
| 5 occlusion culling | medium | nodes | rasterization |
| 6 two frames in flight | medium | -2 MiB | frame time |
| 7 chunked nodes | high | up to -21 MiB | rasterization |
| 8 minor | low | small | small |
| 9 clean region skip (needs 3) | medium | - | binning (cache on) |

For every step : build, `./build/test --autoexit --compare` passes, before/after timings and `gpu_memory_usage` recorded here.
A step that doesn't measure better is reverted and written down in `failed_experiments.md`.
