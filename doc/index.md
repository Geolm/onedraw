# onedraw : a 2d gpu driven renderer

| Section | Description |
|---------|-------------|
| [Overview](index.md) | Goals and Initial Architecture |
| [Part 1](part1.md) | Binning commands |
| [Part 2](part2.md) | Hierarchical binning and intrinsic tricks |
| [Part 3](part2.md) | SDF Group, smoothmin |
| [Part 4](part3.md) | Rasterization |
| [Part 5](part4.md) | Font and textured quads |


## Goals and Initial Architecture

I started the project with the following objectives:

* Not triangle-based: shapes are defined using signed distance functions (SDFs).
* High quality: anti-aliased edges by default, perfectly smooth curves (no tessellation required), optimized for high-resolution displays.
* Fast and GPU-driven: offload as much work as possible to the GPU and minimize draw calls.
* Efficient alpha blending: designed to make extensive use of transparency without significant performance cost.
