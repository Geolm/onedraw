# onedraw — a GPU-driven 2D renderer


## Font

Font rendering is currently quite basic and primarily built on top of [stb_truetype.h](https://github.com/nothings/stb), though it could be adapted to another library.  
During the pre-build phase, the font is loaded, an atlas is generated, compressed to BC4, and the glyph data is extracted. All of this is then exported into header files (`default_font_atlas.h` and `default_font.h`).  
When `onedraw` is initialized, both the atlas texture and glyph data are uploaded to the GPU.

At runtime, when calling `od_draw_char()`, a draw command is pushed using a quad adjusted to the glyph metrics and the character index. In the fragment shader, the UV coordinates are computed and used to sample the atlas texture.

We plan to extend the API to let users provide their own fonts. As for SDF fonts, we’re not opposed to them, but there’s currently no widely accepted standard (for instance, `stb_truetype` only exports distance values, which can introduce artifacts).  
Simplicity is a core design goal of this project, and we want to avoid forcing users to rely on specific atlas generation tools.

## Textured quad

We support bitmap rendering using a texture array. When `onedraw` is initialized, the user can specify the texture array dimensions and the number of slices (up to 256).  
Slices can be uploaded at any time, but since textures use shared memory, updating a slice while it’s being sampled by the GPU may lead to flickering or corruption. It’s the user’s responsibility to handle synchronization when uploading.

At runtime, when calling `od_draw_quad()` or `od_draw_oriented_quad()`, the UVs of the quad are passed along with the slice index. In the rasterizer shader, the texture is then sampled similarly to how font rendering works.

We plan to allow users to configure texture filtering in the future. Currently, linear filtering is used by default.


---

[Next part](part5.md) : Stats and next steps

