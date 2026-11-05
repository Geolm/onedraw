# Plan : offscreen rendering (render to a texture)

Status : proposal, not implemented.

## Goal

Let the renderer produce a frame **without a `CA::MetalDrawable`** : the frame is rendered into a
library-owned offscreen texture instead of a window backbuffer. Primary use case is **unit testing**
(render a scene headlessly, read the pixels back on the CPU, compare against a reference), but it also
serves any host that wants onedraw's output as a texture (compositing, capture, image generation).

## Current state

`od_end_frame(r, drawable)` assumes a drawable end to end (`lib/onedraw.cpp`, `od_flush`) :

1. asserts the drawable texture size matches the renderer,
2. builds the render pass with the drawable texture as color attachment,
3. blits drawable → screenshot texture when a capture was requested,
4. `presentDrawable()` + `commit()`, and waits when a screenshot must be read back.

The screenshot service (`allow_screenshot`) already solves the *readback* half of the problem : a
library-owned shared BGRA8 texture, filled by a blit, copied out with `getBytes()` after
`waitUntilCompleted()`. Only the *render target* half is drawable-bound.

No shader changes are needed : the rasterizer PSOs target a BGRA8 color attachment (sRGB or not,
per `srgb_backbuffer`) and the pipeline never cares which texture backs it.

## Proposed API (non-breaking)

`lib/onedraw.h` :

```c
typedef struct onedraw_def
{
    ...
    bool allow_screenshot;
    bool srgb_backbuffer;
    bool cache;
    bool allow_offscreen;      // NEW : allocate a library-owned offscreen render target
    ...
} onedraw_def;
```

* `od_end_frame(r, NULL)` — `NULL` drawable renders the frame into the offscreen target instead of a
  window. The rest of the contract (begin/end frame, clip balance, groups) is unchanged. Calling
  `od_end_frame(r, NULL)` when `allow_offscreen` is false logs an error and skips the render
  (assert in debug).
* `void* od_get_offscreen_texture(struct onedraw* r);` — returns the `MTL::Texture*` behind the
  `void*` convention (same as `metal_device` / `drawable`), for hosts that consume the texture in
  their own pipeline. Documented pixel format : BGRA8Unorm, sRGB iff `srgb_backbuffer`.
* CPU readback reuses the existing screenshot service as-is : `od_take_screenshot()` after
  `od_end_frame(r, NULL)` returns the offscreen frame pixels (the capture blit's source simply
  becomes the offscreen texture). A unit test therefore needs `.allow_offscreen = true` and
  `.allow_screenshot = true` in `onedraw_def`.
* Version : 3.1.0 → **3.2.0** (new feature, no breaking change). Adding a struct member at the end of
  `onedraw_def` is source-compatible with designated initializers, which the examples and the header
  style already assume.

## Implementation (`lib/onedraw.cpp`)

Follow the screenshot service pattern throughout; the diff is small (~60–80 lines).

1. **State** — new service member in `struct onedraw`, next to `screenshot` :

   ```c
   // offscreen target
   struct
   {
       MTL::Texture* texture {nullptr};
       bool allocate_resources {false};
   } offscreen;
   ```

2. **Creation/resize** — `od_init_offscreen_resources(r)`, mirroring `od_init_screenshot_resources` :
   2D, shared storage, `RenderTarget | ShaderRead | ShaderWrite`, BGRA8Unorm(_sRGB per
   `srgb_backbuffer`), sized from `r->rasterizer.width/height`. Called at the end of `od_resize()`
   (which `od_init` already calls), so init and window resize both stay correct; the 4096 clamp
   applies for free. `od_init` copies `def->allow_offscreen` next to the
   `allow_screenshot` copy.

3. **`od_flush`** — pick the attachment once at the top :

   ```c
   MTL::Texture* target = drawable ? ((CA::MetalDrawable*)drawable)->texture() : r->offscreen.texture;
   ```

   * size assert only when `drawable != NULL` (the offscreen texture is sized by us and cannot drift),
   * `cd->setTexture(target)`,
   * screenshot blit source = `target` (identical code, works for both paths),
   * `presentDrawable` only when `drawable != NULL`.
   * `commit()` / `waitUntilCompleted()` logic unchanged.

4. **`od_terminate`** — `SAFE_RELEASE(r->offscreen.texture)`.

5. **`od_get_stats`** — add `offscreen.texture->allocatedSize()` to `gpu_memory_usage`.

Memory : one extra shared texture, ≤ 64 MiB at the 4096×4096 max — the same class of cost as the
screenshot texture it is designed to be read through, and only paid when `allow_offscreen` is set.

In-flight semantics : the offscreen texture is a **single** texture while up to 2 command buffers
can be in flight (same as the existing cache texture). The screenshot path already waits for
completion before returning pixels, so the unit-test flow is safe. Hosts consuming the texture
directly via `od_get_offscreen_texture()` must document/ensure the previous frame completed —
state this in the header. A double-buffered pair was considered and rejected (128 MiB at max
resolution for a hazard the stated use case doesn't hit).

## Testing (AGENTS.md requires a regression test per feature)

Extend the existing test binary rather than adding a new executable (the scene in `tests/test.c`
would otherwise have to be duplicated) :

* new `--offscreen` flag in `tests/test.c` :
  * `init()` adds `.allow_offscreen = true` (screenshot stays enabled),
  * `frame()` calls `od_end_frame(renderer, NULL)` instead of the swapchain drawable, and the
    `sc.invalid` skip only applies in drawable mode (a hidden window must not stop offscreen frames),
  * the existing frame-10 screenshot → PNG → PSNR compare flow is untouched, so the same committed
    references validate the offscreen pipeline (same pixel format as the test's non-sRGB
    backbuffer, so the images are expected to be identical; if any diff appears, investigate before
    touching `--update-refs`).

Regression gates :

| Command | Proves |
|---------|--------|
| `./build/test --autoexit --compare` | existing drawable path untouched (required gate) |
| `./build/test --autoexit --compare --offscreen` | new offscreen pipeline, new regression test |
| `./build/test --autoexit --validate` and `./build/test --autoexit --validate --offscreen` | Metal validation layer clean — mandatory because GPU resource / render-target setup changed |
| `./build/test --autoexit --compare --offscreen --cache` | offscreen + tile cache (clean-pso second draw) combo, extra local check |

## Documentation

* `lib/onedraw.h` — document `allow_offscreen`, the `NULL` drawable contract of `od_end_frame`, and
  `od_get_offscreen_texture` (format, single-texture in-flight hazard). The header is the primary API
  doc per project convention.
* `README.md` — one feature bullet ("Offscreen rendering — render into a library-owned texture without
  a window, for headless use and unit testing").
* `doc/FAQ.md` — "Can I render without a window?" entry pointing at the flag + readback flow.

## Alternatives considered

* **User-provided texture** (`od_end_frame` taking an `MTL::Texture*` or a dedicated
  `od_render_to_texture`): more flexible, but pushes format/size/lifecycle/resize management onto the
  user and overloads a `void*` parameter into a magic type switch. The `od_get_offscreen_texture`
  accessor covers the "consume in my own pipeline" need without that cost. Rejected for v1.
* **Reuse the screenshot texture as the render target** when `allow_screenshot` is set : couples two
  features, turns the capture blit into a texture→itself copy, and leaves `allow_screenshot=false`
  hosts without a target. Rejected.

## Out of scope (possible follow-ups)

* A fully window-less test executable (no sokol_app, `MTLCreateSystemDefaultDevice` from a `.mm`
  file). The `--offscreen` flag exercises the identical new code path with an idle window, so it is
  not needed for this CL; revisit if a real headless harness is wanted.
* User-selectable offscreen pixel format / mip levels.
* sRGB offscreen is covered by the `srgb_backbuffer` flag but, like the sRGB backbuffer path, is not
  exercised by the references (the test runs non-sRGB); keep the existing "not tested yet" caveat.

## Change summary

| File | Change |
|------|--------|
| `lib/onedraw.h` | `+allow_offscreen` def member, `+od_get_offscreen_texture`, doc, version 3.2.0 |
| `lib/onedraw.cpp` | `+offscreen` service member, `+od_init_offscreen_resources`, `od_flush` target branch, `od_end_frame` NULL guard, terminate/stats |
| `tests/test.c` | `--offscreen` flag, init/frame wiring (~15 lines) |
| `README.md`, `doc/FAQ.md` | doc entries |
| `CMakeLists.txt` | no change (no new sources) |
