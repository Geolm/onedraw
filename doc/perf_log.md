# perf_log

Profiling is done on a mac studio m2 max 32gb with 30 cores gpu, 2560x1440

## version 2.2.3

### no cache
gpu time (last 2142 frames) : 0.59 ms (binning 0.22 ms, rasterization 0.37 ms)
gpu memory : 60783 kb (peak num commands : 2086)

### cache
gpu time (last 1789 frames) : 0.33 ms (binning 0.23 ms, rasterization 0.09 ms)
gpu memory : 75503 kb (peak num commands : 2086)


## version 2.2.4

### no cache
gpu time (last 2193 frames) : 0.63 ms (binning 0.21 ms, rasterization 0.42 ms)
gpu memory : 38943 kb (peak num commands : 2086)

### cache
gpu time (last 1360 frames) : 0.32 ms (binning 0.22 ms, rasterization 0.10 ms)
gpu memory : 53663 kb (peak num commands : 2086)


## version 2.2.5

### no cache
gpu time (last 6585 frames) : 0.62 ms (binning 0.20 ms, rasterization 0.42 ms)
gpu memory : 38941 kb (peak num commands : 2086)


### cache
gpu time (last 2557 frames) : 0.31 ms (binning 0.22 ms, rasterization 0.09 ms)
gpu memory : 53661 kb (peak num commands : 2086)


## version 2.2.6

### no cache
gpu time (last 3096 frames) : 0.61 ms (binning 0.20 ms, rasterization 0.41 ms)
gpu memory : 38941 kb (peak num commands : 2086)


### cache
gpu time (last 4045 frames) : 0.25 ms (binning 0.17 ms, rasterization 0.08 ms)
gpu memory : 53661 kb (peak num commands : 2086)


## version 3.0.0

measured on an M5 Pro (not the M2 Max of the runs above), test scene, last 40 frames

### no cache
gpu time (last 40 frames) : 1.40 ms (binning 0.31 ms, rasterization 1.08 ms)
gpu memory : 41194 kb (peak num commands : 2019)


### cache
gpu time (last 40 frames) : 1.24 ms (binning 0.46 ms, rasterization 0.75 ms)
gpu memory : 41194 kb (peak num commands : 2019)

the memory gain of the tile cache is not visible in this short run; the M2 Max numbers above remain the reference for the cache comparison.
