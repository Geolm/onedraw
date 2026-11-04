# Failed experiments

## One cache member with a single access qualifier (cached_rendering_optimizations.md)

The plan specified that `draw_cmd_arguments.cache_texture` becomes write-only (`texture_half_rw` ->
`texture2d<half, access::write>`) while `clean_fs` reads the cache through the same struct member.

That combination does not compile :

- `texture2d<half, access::write>` has no `read()` member (verified with a probe shader compiled
  through `xcrun metal` on the installed Metal toolchain).
- There is no base `metal::texture` class in the current `metal_stdlib` to cast through, so a
  write-only member cannot be re-typed as read-only inside `clean_fs`.

Resolution implemented instead : the argument buffer carries two members over the same texture
resource - `cache_texture` (write-only, used by `tile_fs`) and `cache_read` (read-only, used by
`clean_fs`). The plan's "no read_write texture" goal is still met (no shader sees a read_write
access), at the cost of 8 extra bytes in the shared `draw_cmd_arguments` struct.

## Multi-font: no text rendered, everything else looked fine (v3.0.0)

After the multi-font rewrite, `od_draw_text` produced no pixels at all while every other
primitive rendered normally. The symptom was deceptively quiet: the tile lists contained the
character commands, the fragment shader visited their tiles, and the reference comparison
"passed" because the references had been regenerated from the same broken run.

The root cause: the GPU glyph table buffer (`r->font.glyphs`, a shared-storage buffer filled
once at init) was dereferenced from `tile_fs` through the `input.glyphs` pointer but was
**never bound with `useResource`** on the render encoder. Without the binding, Metal's hazard
tracking never made the CPU-side copy visible to the GPU, and every read returned zero — so
`char_size` was zero, the in-rect test divided by zero, and nothing was drawn. The pre-3.0
128-entry table happened to work without the binding (small, freshly allocated buffer, first
fetch), which hid the omission. Fix: `useResource(r->font.glyphs, MTL::ResourceUsageRead)` in
the rasterization encoder. Any buffer dereferenced through a pointer must be bound.

A secondary trap in the same investigation: the pre-build builder runs as a `POST_BUILD`
command of its own executable, so editing a `.metal` file or a font never re-ran it — the
embedded shader headers (`lib/*.h`) silently went stale, and probe code in the shaders never
made it into the test binary. The `add_custom_command` now lists the shaders, fonts and stb
headers as `DEPENDS` and the generated headers as `BYPRODUCTS`, so CMake re-runs the builder
when any input is newer.

## Baker: stbtt oversample fields left at zero (v3.0.0)

The rewritten multi-face baker left `stbtt_pack_range.h_oversample` / `v_oversample` unset
(zero). `stbtt_PackFontRangesRenderIntoRects` copies the range's oversample into the pack
context and uses `recip = 1.0f / oversample` — an infinite scale that renders every glyph to
a zero-size rect with NaN/inf bearings, producing an empty atlas. Fix: set both to `1`
explicitly (no supersampling is used, but the fields must be non-zero).
