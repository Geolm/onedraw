# onedraw — a GPU-driven 2D renderer

## Stats

Here are the stats for the [test program](../tests/test.c) (screenshot in homepage) running on my 14-inch MacBook Pro :

* Resolution: 2440p
* 659 draw commands
* 59,295 KB of GPU memory
* GPU time: 1.38 ms (total)

To be honest, the memory usage is a bit higher than I’d like. This is mainly due to the current use of fixed-size buffers and could be improved by allowing customization to better fit the user’s needs.


## Next steps

If time permits, we'd like to work on these topics in the near future

* custom font support
* filtering support for textured quad
* texture format support for the texture array
* ~~a new approach to render quadratic bezier curves without 3 degrees sdf~~
* clip with shapes : disc, ellipse, etc... can be useful for worldmap miniature and won't cost that much
* gradient (two-colors solid), radial gradient, vertical/horizontal gradient
