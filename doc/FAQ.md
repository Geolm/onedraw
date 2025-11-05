# Frequently Asked Questions

### Why Metal only?
At the moment, I only have access to a MacBook Pro, and among modern graphics APIs, **Metal** offers a great balance between simplicity and performance.  
It’s straightforward to work with, yet still powerful enough for advanced GPU-driven techniques.

### Why isn’t it a single-header library?
That’s a deliberate design choice.  
While it would be possible to embed all shaders and headers directly into `onedraw.cpp`, it would make the file harder to read and maintain.  
Instead, the library remains a **single C++ source file** to add to your build system, while keeping shaders and headers separate for clarity.

### What are the by-design limits of the renderer?
We support up to :
* 65536 draw commands (including begin/end group)
* 256 clip rects
* max viewport resolution of 4096x4096
* 256 slices texture array

### What is the coordinate system used?

Coordinates are expressed in pixels, with the x-axis increasing to the right and the y-axis increasing downward.

### Does the library cull objects?

Objects outside the screen are not rasterized, but draw commands are still issued. We recommend implementing a high-level culling system if many objects fall outside the screen to avoid wasting draw calls and GPU resources. Note: There is currently no mechanism to detect redundant primitives that will be completely overwritten by an opaque primitive, but we are exploring optimizations to address this.
