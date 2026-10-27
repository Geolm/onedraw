# Refactor: remove duplicated command-emission code in lib/onedraw.cpp

## Context
Every primitive in `lib/onedraw.cpp` repeats the same ~14 lines. This is the dominant duplication in the file
(13 sites between lines 1159 and 1763), and the dashed-shape code repeats a few smaller blocks. 

Pure refactor: the CPU must produce **byte-identical** command, color, data and aabb buffers for the same calls.
Estimated saving ~250 of 2540 lines.

## 1. One emission helper (~170 lines saved)
Today each of begin_group, end_group, disc, oriented box, ellipse, triangle, pie, arc, box, blurred box, char, quad,
oriented quad does:
```
if (!has_space_for_draw_command(r, n)) { od_log(...); return; }
cmd = buffer.NewElement(); color = colors.NewElement();
cmd->clip_index = LAST_CLIP_INDEX; cmd->data_index = data_buffer.GetNumElements();
cmd->fillmode = ..; cmd->type = ..; (cmd->extra = ..;) *color = ..;
data = data_buffer.NewMultiple(n); aabox = aabb_buffer.NewElement();
write_float(data, ...);
write_quantized_aabb(aabox, bb.min.x, bb.min.y, bb.max.x, bb.max.y);
merge_quantized_aabb(r->commands.group_aabb, aabox);
```
Replace with two small functions:
```
// low level : space check + log, command, color, clip index, data reservation. Returns the data pointer (nullptr if full)
// and the aabb slot, nothing is written into it. Used by begin_group / end_group, which handle their aabb themselves.
static float* new_command(struct onedraw* r, enum command_type type, enum primitive_fillmode fillmode, uint8_t extra,
                          draw_color color, uint32_t num_data, quantized_aabb** out_aabb);

// primitives : new_command + write the quantized box + merge into the open group. Returns the data pointer or nullptr.
static float* new_primitive(struct onedraw* r, enum command_type type, enum primitive_fillmode fillmode, uint8_t extra,
                            draw_color color, uint32_t num_data, aabb bb);
```
Each primitive then reads: validate, compute `bb`, `float* data = new_primitive(...); if (data == nullptr) return;`,
`write_float(data, ...)`. The out-of-space log lives in one place.

Quirks the helper must preserve (each one is a place a careless merge changes output):
- `bb` is passed already grown by `draw_cmd_aabb_bump(r)` where the function does it today. Blurred box, char and quad
  do **not** add the bump; do not "fix" that here.
- begin_group keeps `group_aabb` pointing at the slot it reserved (`*out_aabb`) and fills it with
  `invalid_quantized_aabb()`; end_group copies `*group_aabb` into its slot. Neither merges into a group.
- Space is checked before any element is taken. Early returns that precede the check (zero-size shapes, invalid
  char, null atlas) stay in the callers, in the same order.
- `has_space_for_draw_command` stays public to the file: the dash emitter uses it to avoid the log.
- `write_quantized_aabb` takes an `aabb` instead of four floats (the only callers are the helpers).
- `LAST_CLIP_INDEX` is used once, in `new_command`. This is what makes the clip plan's 13-site rename disappear, and
  the transform plan's per-command writes (id, margin) go in the same place.

## 2. Dashed shapes (~35 lines saved, byte-identical)
- `if (round_caps) od_draw_capsule(...) else od_draw_line(...)` appears three times (dashed line zero-gap,
  `dash_emitter_emit`, the needle ellipse). One `static void draw_dash(r, a, b, width, round_caps, color)`.
- Every closed-path caller ends with `path.perimeter = dash_path_polyline_perimeter(&path); return dash_path_draw(...)`
  (ring, box, oriented box, ellipse, triangle). One `dash_closed_path_draw(r, &path, thickness, style, color)`.
- `od_draw_dashed_box` and `od_draw_dashed_oriented_box` differ only in how they compute center, axis, half extents and
  corner; the zero-gap `dash_length` fix-up (with its 2-line comment, duplicated), the path build and the draw are
  identical. They share a `dash_box(...)` tail.
- The `if (style.dash_length <= 0.f) return 0;` that precedes `dash_path_draw` in six callers is repeated at the top of
  `dash_path_draw` itself. Delete the six. Keep the zero-gap solid branches before it (a gap of 0 draws the solid
  shape even if `dash_length` is 0).


