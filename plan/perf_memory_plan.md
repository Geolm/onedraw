# Performance and memory plan


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

## Step 10 : `tile_bin` list + aabb in threadgroup memory (after step 9)

### Why

The `tile_bin` walk (one thread per tile, one threadgroup of 256 threads per region) runs a per-iteration chain of
`indices[i]` -> `commands_aabb[cmd]` -> `commands[cmd]` -> `draw_data` -> SDF ALU. All the loads are uniform per warp : the 256 threads of
a threadgroup walk the same region list in lockstep (`i` is identical across active lanes, `simd_broadcast_first` already relies on it), so
the GPU issues one request per warp per buffer. This is not a bandwidth problem - the aabb buffer is ~8 KiB at the scene peak, streamed by
all 60 region threadgroups and stays in cache. It is a dependent-latency problem : the aabb is the first data-dependent load of the
iteration, and in the test scene most iterations die on the aabb integer test right after it. Staging the list and the aabbs in threadgroup
memory removes those round-trips from the critical path of every iteration.


### Budget

Apple caps threadgroup memory at 32 KiB per threadgroup and the array size must be a compile-time constant :

| staged | bytes / entry | entries in 32 KiB |
|--------|---------------|-------------------|
| list (uint16) only | 2 | 16384 |
| aabbs (quantized_aabb) only | 4 | 8192 |
| list + aabbs | 6 | 5461 |

`STAGE = 4096` entries : 8 KiB + 16 KiB = 24 KiB, 1.9x the scene peak of 2086 commands. List-only is the budget-lean variant but the weak
one (the `indices[i]` address is `base + 2*i`, a static offset the compiler can already pipeline ahead) ; aabb-only keeps the full 8192 but
halves the gain at the same protocol cost. List + aabb is the variant to build.

### Staging protocol (1 barrier, no length scan, no scratch)

1. Gate before the walk : `bool staged = input.num_commands <= STAGE;`. One scalar, uniform over the threadgroup and the grid : the branch
   diverges nowhere (one compare per kernel entry), and it is what makes the barrier below reachable by every thread.
2. Placement : the staging + barrier go **before** the early `return` of the off-screen edge tiles (`binning.metal:654`) : a thread that
   returned would never reach the barrier. Edge threads pay one unused staging pass per frame, negligible.
3. Each of the 256 threads stages its own chunk (`ceil(num_commands / 256)` <= 16 entries, in-threadgroup index `thread_pos.y * 16 + x` -
   the 16x16 threadgroup tiles the grid 1:1 with the region, no new parameter), both phases pure global-to-threadgroup on its own chunk :
   - `list[k] = indices[k]`
   - `c = list[k]; if (c != LAST_COMMAND) aabbs[k] = commands_aabb[c];` - reads back its own staged chunk (same-thread visibility, no
     barrier between the phases), **position-indexed** : slot k holds the aabb of list entry k, so the walk reads a static address.
   - The stale tail (step 2 removed the per-frame clear, `region_bin` writes only `[prefix][terminator]`) is safe to stage : entries past
     the terminator hold arbitrary uint16s, but the aabb ring is sized `LAST_COMMAND` (`onedraw.cpp:1046`), so any value but the terminator
     is in-bounds. Worst case, garbage aabbs land at slots the walk never reaches (it breaks at the terminator) : never read, `--validate`
     clean. No length reduction needed.
4. One `threadgroup_barrier(mem_flags::mem_threadgroup)` after the phases, before the walk.
5. Walk : the two load sites become `if (staged) <threadgroup load> else <current constant buffer load>` - an **if/else, not a ternary** :
   a ternary can codegen to a select that issues both loads and silently voids the gain. The branch is loop-invariant and uniform, ~1 cycle
   per site per iteration. The single loop body is shared by both paths : no code duplication, register and icache pressure stay at the max
   of the two paths (the SDF core dominates and is identical).
6. Fallback (`num_commands > STAGE`) : the current code untouched. The 24 KiB reservation is per dispatch, so it applies to fallback frames
   too : a few fewer threadgroups resident per core, irrelevant at 60 threadgroups of 256 threads. A two-PSO design (the host picks) would
   erase even the branch and the reservation, for a second pipeline and host path : not worth it.

### Host side

- `src/shaders/common.h` : `#define STAGE_COMMANDS 4096` (the pre-build copies it to `lib/common.h` and regenerates `lib/binning.h`,
  committed with the change).
- `od_bin_commands` : `setThreadgroupMemoryLength(STAGE_COMMANDS * (2 + 4), 0)` on the tile_bin dispatch (24576, a multiple of 16). The two
  new `[[threadgroup(0)]]` / `[[threadgroup(1)]]` parameters are bound by the length, no `setBuffer` ; the `newArgumentEncoder(0)` / `(1)`
  calls of `od_build_pso` keep targeting buffers 0/1, the buffer indices do not move.

### Validation

- `--compare` : identical logic and order, the images must not move (a sanity gate, not the point).
- `--validate` : the in-bounds argument of 3.
- The regression scene peaks at 2086, so it only exercises the staged path : the fallback needs a temporary >4096-command scene
  (`--compare` + `--validate`, like the 1000-discs scene of step 3) or it rots silently.
- Measure before/after : `binning` and `gpu`, cache off and on, ~2 min runs (short runs drift with machine state, and the machine shows
  run-to-run variance, a flat result gets a second look). If flat : first suspect the load codegen (dump the MSBC, the aabb load must be
  gone from the staged loop) ; if the load is gone and it is still flat, the loads were already pipelined and the step goes to
  `failed_experiments.md`.
