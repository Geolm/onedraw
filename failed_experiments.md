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
