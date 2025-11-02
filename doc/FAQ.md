# Frequently Asked Questions

###### Why Metal only?
At the moment, I only have access to a MacBook Pro, and among modern graphics APIs, **Metal** offers a great balance between simplicity and performance.  
It’s straightforward to work with, yet still powerful enough for advanced GPU-driven techniques.

###### Why isn’t it a single-header library?
That’s a deliberate design choice.  
While it would be possible to embed all shaders and headers directly into `onedraw.cpp`, it would make the file harder to read and maintain.  
Instead, the library remains a **single C++ source file** to add to your build system, while keeping shaders and headers separate for clarity.

###### What are the by-design limits of the renderer
We support up to :
* 65536 draw commands (including begin/end group)
* 256 clip rects
* max viewport resolution of 4096x4096
* 256 slices texture array

