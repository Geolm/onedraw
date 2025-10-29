# onedraw
**GPU-driven 2D renderer drop-in library**

`onedraw` is designed to render everything in a single draw call, maximizing GPU efficiency for 2D graphics. More details can be found in the [documentation](doc/index.md).

### Features
* **Single draw call rendering** – as the name suggests, all primitives are drawn in one draw call.
* **Hierarchical tile-based binning** – the compute shader processes only the primitives that influence each tile, reducing wasted work.
* **Shader-based alpha blending** – all blending operations are handled in the shader, no framebuffer read/write required.
* **Anti-aliasing** – smooth edges using signed distance functions.
* **Lightweight and minimal** – drop-in library with minimal dependencies.
* **Baked fonts** – ready-to-use text rendering (custom fonts are planned).
* **Wide shape support** – box, blurred box, rectangle, oriented box/rectangle, triangle, triangle ring, disc, circle, ellipse, arc, sector, textured quad, oriented textured quad.
* **Shape operations** – shapes can be grouped (boolean add), and [smooth minimum](https://iquilezles.org/articles/smin/) is supported for more organic shapes.

### Integration
1. Copy all files from the `/lib` folder.  
2. Add `onedraw.cpp` to your build system.  
3. Create your window and provide the Metal device and drawable object.  

See [tests/test.c](tests/test.c) for a full example using `sokol_app.h`.

### Project structure


```C

onedraw
 |
 |--src         source files that will produce the one-cpp library
 |
 |--lib         the library to be included in a project
 |
 |--pre-build   source code to generate font, shader, library
 |
 |--tests       simple test with sokol_app, built after each commit
 |

```
