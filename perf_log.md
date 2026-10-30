# Performance & memory history

Cross-version measurements on identical conditions, to compare releases.
The per-step in-development numbers live in `plan/perf_memory_plan.md` ; the per-pass splits
(binning / rasterization) are version-local and stay there too, because the pass composition
changed with the fused region binning kernel and would not be comparable across versions.

## Protocol (keep identical when appending a version)

- machine : MacBook Pro, M5 Pro
- resolution : 2560x1440 (160x90 tiles, 10x6 regions)
- scene : the beefed test scene (06c0ec3 / 08b1028, peak ~2044 commands), ported to every version
- gpu ms : the test.c average from frame 61 to quit ; use a ~2 min run, short runs drift up to ~0.4 ms with machine state
- gpu memory : the `od_get_stats` sum of allocations ; deterministic, scene-independent up to the 65534 command cap
- the cache on row is +14.6 MiB in every version (the full-frame cache texture) ; compare like with like

| version | cache | gpu ms | gpu memory |
|---------|-------|--------|------------|
| v2.0.0  | off   | 2.45   | 74661 kb (72.9 MiB) |
| v2.0.0  | on    | 1.65   | 89637 kb (87.5 MiB) |
| v2.2.2  | off   | 2.09   | 61087 kb (59.7 MiB) |
| v2.2.2  | on    | 1.50   | 76063 kb (74.3 MiB) |

## Deltas

- v2.0.0 -> v2.2.2 : frame time **-15 %** (no cache) / **-9 %** (cache) ; gpu memory **-13.3 MiB** at 1440p (~-27 MiB at 4K)
  - memory : 11.25 MiB, the fused region binning kernel removed the scan + predicate buffers (plan step 3) ; 2 MiB, cpu rings x3 -> x2 (plan step 6)
  - frame time : the fused region binning kernel, the deleted per-frame `regions.indices` clear, the leaner `tile_bin`, and the cpu/gpu overlap from two frames in flight
