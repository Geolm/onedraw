#!/usr/bin/env python3
"""Regenerates the doc/ SVG diagrams that describe the current renderer.

Run from the project root:  .docgen/venv/bin/python doc/gen_diagrams.py
The diagrams are plain SVG (no dependencies), styled after doc/coordinates.svg.
"""
import os

FONT = "Helvetica, Arial, sans-serif"

# palette (matches the old chart pngs / coordinates.svg)
INK = "#1E293B"
MUTED = "#64748B"
CARD = "#F8FAFC"
BORDER = "#E2E8F0"
PALETTE = {
    "purple": ("#7B72E3", "#E9E6FB"),
    "blue":   ("#3D6BD1", "#DBEAFE"),
    "green":  ("#2F9E63", "#D7F0E2"),
    "orange": ("#DD8500", "#FDEBD3"),
    "red":    ("#D25B53", "#F9DCD9"),
    "teal":   ("#2A9D8F", "#D6F0ED"),
    "grey":   ("#64748B", "#EEF1F5"),
}


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


class Svg:
    def __init__(self, w, h, title=None, subtitle=None, footer=None):
        self.w, self.h = w, h
        self.parts = []
        self.parts.append(
            f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}" role="img">')
        self.parts.append(f'<rect x="0" y="0" width="{w}" height="{h}" rx="16" fill="white" stroke="{BORDER}" stroke-width="1.5"/>')
        if title:
            self.text(w / 2, 42, title, 20, INK, "middle", "bold")
        if subtitle:
            self.text(w / 2, 68, subtitle, 13.5, MUTED, "middle")
        if footer:
            self.text(w / 2, h - 22, footer, 13, MUTED, "middle")

    def text(self, x, y, s, size=13, fill=INK, anchor="start", weight="normal"):
        self.parts.append(
            f'<text x="{x:.1f}" y="{y:.1f}" font-size="{size}" fill="{fill}" text-anchor="{anchor}" '
            f'font-family="{FONT}" font-weight="{weight}">{esc(s)}</text>')

    def rect(self, x, y, w, h, fill, rx=8, stroke=None, sw=1.5, dash=None):
        d = f' stroke-dasharray="{dash}"' if dash else ""
        s = f' stroke="{stroke}" stroke-width="{sw}"' if stroke else ""
        self.parts.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" rx="{rx}" fill="{fill}"{s}{d}/>')

    def line(self, x1, y1, x2, y2, stroke=INK, sw=1.5, dash=None):
        d = f' stroke-dasharray="{dash}"' if dash else ""
        self.parts.append(f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" stroke="{stroke}" stroke-width="{sw}"{d}/>')

    def arrow(self, x1, y1, x2, y2, stroke=INK, sw=2, head=7):
        self.line(x1, y1, x2, y2, stroke, sw)
        import math
        ang = math.atan2(y2 - y1, x2 - x1)
        p1 = (x2 - head * math.cos(ang - 0.45), y2 - head * math.sin(ang - 0.45))
        p2 = (x2 - head * math.cos(ang + 0.45), y2 - head * math.sin(ang + 0.45))
        self.parts.append(f'<polygon points="{x2:.1f},{y2:.1f} {p1[0]:.1f},{p1[1]:.1f} {p2[0]:.1f},{p2[1]:.1f}" fill="{stroke}"/>')

    def card(self, x, y, w, h, label=None, label_color=None, rx=12):
        self.rect(x, y, w, h, CARD, rx=rx, stroke=BORDER, sw=1.5)
        if label:
            self.text(x + w / 2, y + 26, label, 15, label_color or INK, "middle", "bold")

    def chip(self, x, y, w, h, text, color, label=None):
        dark, light = PALETTE[color]
        self.rect(x, y, w, h, light, rx=h / 2, stroke=dark, sw=1.6)
        self.text(x + (w / 2 if label is None else 12), y + h / 2 + 4.5, text, 12.5,
                  dark if label is None else INK, "middle" if label is None else "start", "bold" if label is None else "normal")

    def numbered_box(self, x, y, w, h, num, title, lines, color):
        dark, light = PALETTE[color]
        self.rect(x, y, w, h, light, rx=10, stroke=dark, sw=1.8)
        self.parts.append(f'<circle cx="{x + 16}" cy="{y + 17}" r="9" fill="{dark}"/>')
        self.text(x + 16, y + 21, str(num), 11, "white", "middle", "bold")
        self.text(x + 32, y + 22, title, 13.5, INK, "start", "bold")
        ty = y + 40
        for ln in lines:
            self.text(x + 32, ty, ln, 12, MUTED)
            ty += 16

    def save(self, path):
        self.parts.append("</svg>")
        with open(path, "w") as f:
            f.write("\n".join(self.parts) + "\n")
        print("wrote", path)


# =============================================================================
# GPU memory chart (doc/memory.svg) — numbers at 1440p, cache off, v2.2.4
# =============================================================================
def gen_memory():
    W, H = 1160, 660
    s = Svg(W, H,
            title="GPU memory at 1440p (2560\u00d71440) \u00b7 v2.2.4",
            subtitle="every buffer is fixed-capacity, so this is a stable upper bound (measured with od_get_stats)",
            footer="the screenshot buffer is optional \u2014 the first thing to drop \u00b7 with the tile cache on the total grows to \u2248 53 MB")

    # category: (label, sublabel, MiB, pct, color, mb_label)
    cats = [
        ("screenshot",        "framebuffer capture (only if allow_screenshot)",          14.0, 37, "red"),
        ("per-tile node pool","699,051 \u00d7 16-byte chunks (6 command slots each), fixed", 10.7, 28, "purple"),
        ("region lists",      "indices (scales with resolution)",                          7.5, 20, "blue"),
        ("per-frame CPU ring","commands, colors, data, AABBs, clips \u00b7 2 in flight",    4.0, 11, "green"),
        ("other",             "texture array, command hashes, tile bookkeeping, font atlas", 1.9,  5, "orange"),
    ]
    total_kib = 38943
    s.text(60, 112, "\u2248 39 MB total (38,943 kb)", 15, INK, "start", "bold")

    bar_x, bar_y, bar_w, bar_h = 60, 126, 1040, 34
    x = bar_x
    for label, sub, mb, pct, color in cats:
        seg_w = pct / 100.0 * bar_w
        dark, light = PALETTE[color]
        s.rect(x, bar_y, seg_w, bar_h, dark, rx=0)
        if seg_w > 60:
            s.text(x + seg_w / 2, bar_y + 22, f"{pct}%", 12.5, "white", "middle", "bold")
        x += seg_w
    s.rect(bar_x, bar_y, bar_w, bar_h, "none", rx=8, stroke=BORDER, sw=1)

    y = 196
    for label, sub, mb, pct, color in cats:
        s.card(60, y, 1040, 72)
        s.text(84, y + 30, label, 15.5, INK, "start", "bold")
        s.text(84, y + 52, sub, 12.5, MUTED)
        bar_max = 14.0
        w = mb / bar_max * 360
        dark, light = PALETTE[color]
        s.rect(600, y + 28, w, 16, dark, rx=8)
        s.text(600 + w + 14, y + 41, f"{mb:g} MB \u00b7 {pct}%", 13.5, INK, "start", "bold")
        y += 80

    s.save("doc/memory.svg")


# =============================================================================
# How onedraw works (doc/overview.svg)
# =============================================================================
def gen_overview():
    W, H = 1280, 840
    s = Svg(W, H,
            title="How onedraw works",
            subtitle="a flat command list becomes per-tile lists, then per-tile fragments")

    # -- panel 1 : CPU writes draw commands -----------------------------------
    s.card(60, 110, 560, 300, "1 \u00b7 CPU writes draw commands", PALETTE["purple"][0])
    s.rect(110, 168, 460, 218, "white", rx=10, stroke=INK, sw=1.8)
    s.text(340, 196, "command ring buffer (double-buffered)", 13, MUTED, "middle")
    chips = [("#0 box", "blue"), ("#1 text", "purple"), ("#2 disc", "green"), ("#3 ellipse", "teal"), ("#4 quad", "grey")]
    cy = 212
    for label, color in chips:
        s.chip(140, cy, 400, 28, label, color)
        cy += 33

    # -- panel 2 : build per-tile lists ---------------------------------------
    s.card(660, 110, 560, 300, "2 \u00b7 build per-tile lists", PALETTE["blue"][0])
    gx, gy, gs = 800, 168, 26
    dots = {(1, 1), (2, 1), (4, 2), (1, 3), (3, 3), (5, 4), (3, 5)}
    for ty in range(6):
        for tx in range(6):
            x, y = gx + tx * gs, gy + ty * gs
            s.rect(x + 1, y + 1, gs - 2, gs - 2, "#F1F5F9", rx=2, stroke="#CBD5E1", sw=1)
            if (tx, ty) in dots:
                s.parts.append(f'<circle cx="{x + gs / 2}" cy="{y + gs / 2}" r="4" fill="{PALETTE["blue"][0]}"/>')
    s.text(878, 342, "each tile holds only the commands that touch it", 12.5, MUTED, "middle")

    s.arrow(622, 260, 658, 260, sw=2.2)
    s.text(640, 246, "GPU compute", 12, MUTED, "middle")

    # -- panel 3 : why per-tile lookup matters --------------------------------
    s.card(60, 440, 560, 330, "why per-tile lookup matters")
    s.text(100, 492, "Without it, every fragment would test", 13.5, INK)
    s.text(100, 512, "every command against its pixel.", 13.5, INK)
    s.text(100, 556, "With it, a fragment only evaluates the", 13.5, INK)
    s.text(100, 576, "handful of commands that actually", 13.5, INK)
    s.text(100, 596, "reach its 16\u00d716 tile \u2014 so expensive", 13.5, INK)
    s.text(100, 616, "SDFs are computed only where needed.", 13.5, INK)

    # -- panel 4 : rasterize the per-tile list --------------------------------
    s.card(660, 440, 560, 330, "3 \u00b7 rasterize the per-tile list", PALETTE["green"][0])
    s.rect(700, 500, 150, 150, "white", rx=4, stroke=INK, sw=1.8)
    s.parts.append(f'<circle cx="760" cy="570" r="34" fill="{PALETTE["green"][1]}" stroke="{PALETTE["green"][0]}" stroke-width="2"/>')
    s.rect(780, 590, 70, 55, PALETTE["blue"][1], rx=2, stroke=PALETTE["blue"][0], sw=2)
    s.rect(800, 648, 110, 6, PALETTE["orange"][0], rx=2)
    s.text(912, 512, "node_chunk (16 B)", 12, MUTED, "start", "bold")
    s.text(912, 528, "6 command slots + next link", 11, MUTED)
    for i, (label, color) in enumerate([("#0 disc", "green"), ("#1 ellipse", "teal"), ("#2 box", "blue")]):
        s.chip(908, 536 + i * 30, 240, 26, label, color)
    s.parts.append(f'<circle cx="1028" cy="632" r="3" fill="{MUTED}"/>')
    s.text(1030, 626, "next", 11, MUTED)
    s.text(908, 664, "SDFs evaluated in order \u00b7 alpha blended \u00b7 group ops", 12, MUTED)

    s.arrow(940, 412, 940, 438, sw=2.2)
    s.text(952, 422, "one indirect draw of active tiles", 12, MUTED)
    s.text(952, 436, "(a second one redraws cached tiles when the cache is on)", 11, MUTED)

    s.save("doc/overview.svg")


# =============================================================================
# GPU pipeline (doc/pipeline.svg)
# =============================================================================
def gen_pipeline():
    W, H = 1440, 680
    s = Svg(W, H,
            title="GPU-driven pipeline",
            subtitle="a short chain of compute kernels, ending in one indirect draw (two when the tile cache is on)",
            footer="no full-screen pass and no CPU work after od_end_frame() \u2014 the GPU decides what to draw")

    def stage(x, num, title, lines, color):
        dark, light = PALETTE[color]
        s.rect(x, 150, 300, 110, light, rx=12, stroke=dark, sw=1.8)
        s.parts.append(f'<circle cx="{x + 20}" cy="170" r="10" fill="{dark}"/>')
        s.text(x + 20, 174, str(num), 11.5, "white", "middle", "bold")
        s.text(x + 38, 175, title, 15.5, INK, "start", "bold")
        ty = 200
        for ln in lines:
            s.text(x + 38, ty, ln, 12, MUTED)
            ty += 16

    stage(70,  0, "draw commands", ["CPU writes a flat list", "into a double-buffered ring"], "purple")
    stage(400, 1, "hash commands", ["one content hash per command,", "feeds the tile cache"], "teal")
    stage(730, 2, "region bin", ["fused predicate + scan + scatter,", "one threadgroup per region"], "green")
    stage(1060, 3, "tile bin", ["walk the region list per tile,", "build the chunk list, head in a register"], "orange")

    for x in (372, 702, 1032):
        s.arrow(x, 205, x + 26, 205, sw=2.2)

    # stage 4 : write ICB, under the tile bin
    dark, light = PALETTE["red"]
    s.rect(1060, 380, 300, 110, light, rx=12, stroke=dark, sw=1.8)
    s.parts.append(f'<circle cx="1080" cy="400" r="10" fill="{dark}"/>')
    s.text(1080, 404, "4", 11.5, "white", "middle", "bold")
    s.text(1098, 405, "write ICB", 15.5, INK, "start", "bold")
    s.text(1098, 430, "count the dirty and the clean tiles,", 12, MUTED)
    s.text(1098, 446, "write the two indirect buffers", 12, MUTED)

    s.arrow(1210, 262, 1210, 378, sw=2.2)

    # the two indirect draws, forked below the write ICB
    s.rect(790, 530, 230, 100, PALETTE["red"][1], rx=12, stroke=PALETTE["red"][0], sw=1.8)
    s.text(905, 563, "indirect draw", 14.5, INK, "middle", "bold")
    s.text(905, 583, "dirty tiles", 13, PALETTE["red"][0], "middle", "bold")
    s.text(905, 603, "re-shade every tile that changed", 11.5, MUTED, "middle")
    s.text(905, 619, "and store it in the cache (when on)", 11.5, MUTED, "middle")

    s.rect(1080, 530, 230, 100, PALETTE["green"][1], rx=12, stroke=PALETTE["green"][0], sw=1.8, dash="7 5")
    s.text(1195, 563, "indirect draw", 14.5, INK, "middle", "bold")
    s.text(1195, 583, "clean tiles (opt-in cache)", 13, PALETTE["green"][0], "middle", "bold")
    s.text(1195, 603, "redraw the unchanged tiles from", 11.5, MUTED, "middle")
    s.text(1195, 619, "the cache, the walk is skipped", 11.5, MUTED, "middle")

    s.arrow(1130, 492, 905, 528, sw=2.2)
    s.arrow(1290, 492, 1195, 528, sw=2.2)

    s.save("doc/pipeline.svg")


# =============================================================================
# Empty groups (doc/group-empty.svg)
# =============================================================================
def gen_group_empty():
    W, H = 1120, 600
    s = Svg(W, H,
            title="Empty groups are cleaned per tile",
            subtitle="if no group shape reaches a tile, the begin/end pair is removed")

    # tile A : group reaches it
    s.card(60, 120, 560, 170)
    s.text(84, 152, "tile A \u00b7 group reaches it", 14, INK, "start", "bold")
    chips = [("begin", "purple"), ("disc", "green"), ("box", "blue"), ("end", "purple")]
    x = 84
    for label, color in chips:
        s.chip(x, 180, 110, 34, label, color)
        x += 122
    s.text(540, 252, "kept (group has geometry)", 13, PALETTE["green"][0], "end", "bold")

    # tile B : group does not reach it
    s.card(60, 320, 560, 170)
    s.text(84, 352, "tile B \u00b7 group does not reach it", 14, INK, "start", "bold")
    for i, label in enumerate(["begin", "end"]):
        x = 84 + i * 122
        s.chip(x, 380, 110, 34, label, "grey")
        s.line(x, 414, x + 110, 380, PALETTE["red"][0], sw=2)
    s.text(540, 452, "begin/end dropped \u2014 nothing left to draw", 13, PALETTE["red"][0], "end", "bold")

    # right panel : what tile_bin does
    s.card(660, 120, 400, 370, "tile_bin walks the region list")
    s.text(690, 180, "in reverse order, tracking the", 13.5, INK)
    s.text(690, 200, "group's extra margin; the", 13.5, INK)
    s.text(690, 220, "begin/end pair is only emitted", 13.5, INK)
    s.text(690, 240, "once a primitive of the group", 13.5, INK)
    s.text(690, 260, "actually reaches the tile \u2014", 13.5, INK)
    s.text(690, 280, "a group that wraps zero", 13.5, INK)
    s.text(690, 300, "primitives produces no nodes,", 13.5, INK)
    s.text(690, 320, "no separate clean-up pass.", 13.5, INK)

    s.save("doc/group-empty.svg")


# =============================================================================
# Font pipeline (doc/font-pipeline.svg)
# =============================================================================
def gen_font_pipeline():
    W, H = 1200, 620
    s = Svg(W, H,
            title="Font pipeline",
            subtitle="baked at build time, uploaded at init, sampled at draw time")

    def row_flow(y, steps, first_grey=True):
        x = 70
        for i, (label, color) in enumerate(steps):
            c = "grey" if (first_grey and i == 0) else color
            s.rect(x, y, 190, 52, PALETTE[c][1], rx=10, stroke=PALETTE[c][0], sw=1.8)
            s.text(x + 95, y + 31, label, 13.5, INK, "middle", "bold")
            if i < len(steps) - 1:
                s.arrow(x + 192, y + 26, x + 228, y + 26, sw=2)
            x += 230

    s.text(150, 128, "pre-build (build time)", 14, PALETTE["purple"][0], "start", "bold")
    row_flow(150, [("4 fonts x 9 sizes", "grey"), ("stb_truetype", "blue"), ("one atlas (BC4)", "green"), ("headers", "orange")])
    s.line(215, 208, 215, 258, MUTED, 1.5, "6 5")
    s.text(228, 228, "-> default_font_atlas.h + default_font.h (36 faces, 199 glyphs, plain C, no runtime dep)", 12, MUTED)

    s.text(150, 300, "runtime", 14, PALETTE["green"][0], "start", "bold")
    row_flow(320, [("od_draw_text", "grey"), ("pick face + scale", "blue"), ("char per code point", "green"), ("sample + invert = SDF", "orange")])

    s.card(60, 430, 1080, 148)
    s.text(90, 465, "a face is a (font, pixel size) pair: 4 fonts x 9 sizes = 36 faces share one BC4 atlas, with a 36 x 256-entry", 13, INK)
    s.text(90, 488, "glyph table (216 KiB) uploaded once at init. a requested size picks the smallest baked size at or above the", 13, INK)
    s.text(90, 511, "physical size and scales the glyph by the ratio, so text is only ever minified, never magnified below it.", 13, INK)
    s.text(90, 534, "at draw time the UV is computed from the pixel, the atlas sampled, and the coverage inverted to act as a", 13, INK)
    s.text(90, 557, "distance-like mask, so glyph edges are anti-aliased exactly like every other SDF.", 13, INK)

    s.save("doc/font-pipeline.svg")


# =============================================================================
# Bezier binning (doc/bezier-tiles.svg)
# =============================================================================
def gen_bezier_tiles():
    W, H = 1120, 720
    s = Svg(W, H,
            title="B\u00e9zier binning: one curve, scattered across tiles",
            subtitle="each tile stores only the capsules it touches")

    gx, gy, gs = 110, 130, 58

    # one smooth cubic bezier, tessellated the same way the renderer does:
    # De Casteljau recursion until the control points are near-colinear, one capsule per segment
    p0 = (gx + 1.05 * gs, gy + 4.9 * gs)
    c1 = (gx + 4.55 * gs, gy + 4.9 * gs)
    c2 = (gx + 3.45 * gs, gy + 1.1 * gs)
    p3 = (gx + 6.95 * gs, gy + 1.1 * gs)

    def mid(a, b):
        return ((a[0] + b[0]) / 2, (a[1] + b[1]) / 2)

    def dist_point_line(p, a, b):
        dx, dy = b[0] - a[0], b[1] - a[1]
        length = (dx * dx + dy * dy) ** 0.5
        if length == 0:
            return ((p[0] - a[0]) ** 2 + (p[1] - a[1]) ** 2) ** 0.5
        return abs(dy * p[0] - dx * p[1] + b[0] * a[1] - b[1] * a[0]) / length

    def tessellate(a, ca, cb, b, tol, segs, depth=0):
        if depth > 24 or max(dist_point_line(ca, a, b), dist_point_line(cb, a, b)) < tol:
            segs.append((a, b))
            return
        m1, m2, m3 = mid(a, ca), mid(ca, cb), mid(cb, b)
        n1, n2 = mid(m1, m2), mid(m2, m3)
        m = mid(n1, n2)
        tessellate(a, m1, n1, m, tol, segs, depth + 1)
        tessellate(m, n2, m3, b, tol, segs, depth + 1)

    def seg_hits(a, b, x0, y0, x1, y1):
        t0, t1 = 0.0, 1.0
        dx, dy = b[0] - a[0], b[1] - a[1]
        for p, q in ((-dx, a[0] - x0), (dx, x1 - a[0]), (-dy, a[1] - y0), (dy, y1 - a[1])):
            if p == 0:
                if q < 0:
                    return False
            else:
                t = q / p
                if p < 0:
                    if t > t1:
                        return False
                    if t > t0:
                        t0 = t
                else:
                    if t < t0:
                        return False
                    if t < t1:
                        t1 = t
        return True

    segs = []
    tessellate(p0, c1, c2, p3, 8.0, segs)
    radius = 0.15 * gs
    counts = {}
    for ty in range(6):
        for tx in range(8):
            n = sum(1 for (a, b) in segs
                    if seg_hits(a, b, gx + tx * gs - radius, gy + ty * gs - radius,
                                gx + (tx + 1) * gs + radius, gy + (ty + 1) * gs + radius))
            if n:
                counts[(tx, ty)] = n

    # tile grid
    for ty in range(6):
        for tx in range(8):
            x, y = gx + tx * gs, gy + ty * gs
            if (tx, ty) in counts:
                s.rect(x + 1.5, y + 1.5, gs - 3, gs - 3, "#EFF9F2", rx=3, stroke=PALETTE["green"][0], sw=1.8)
                s.text(x + gs / 2, y + gs / 2 + 5, str(counts[(tx, ty)]), 13, INK, "middle", "bold")
            else:
                s.rect(x + 1.5, y + 1.5, gs - 3, gs - 3, "#F8FAFC", rx=3, stroke="#E2E8F0", sw=1)

    path = f"M {p0[0]:.1f},{p0[1]:.1f} C {c1[0]:.1f},{c1[1]:.1f} {c2[0]:.1f},{c2[1]:.1f} {p3[0]:.1f},{p3[1]:.1f}"
    s.parts.append(f'<path d="{path}" fill="none" stroke="{PALETTE["green"][0]}" stroke-width="13" stroke-linecap="round" opacity="0.55"/>')
    s.parts.append(f'<path d="{path}" fill="none" stroke="{PALETTE["green"][0]}" stroke-width="4" stroke-linecap="round"/>')
    for (x, y) in (p0, p3):
        s.parts.append(f'<circle cx="{x:.1f}" cy="{y:.1f}" r="6" fill="{INK}"/>')

    s.text(378, 512, "number = capsules kept by that tile", 13, MUTED, "middle")

    # right panel
    s.card(700, 130, 360, 380)
    s.text(730, 170, "the curve is tessellated on the CPU", 13.5, INK)
    s.text(730, 190, "into many capsule draw commands", 13.5, INK)
    s.text(730, 210, "(one per segment). binning scatters", 13.5, INK)
    s.text(730, 230, "them so each tile keeps only the", 13.5, INK)
    s.text(730, 250, "capsules that reach it (1\u20133 here).", 13.5, INK)
    s.text(730, 290, "a fragment then evaluates just", 13.5, INK)
    s.text(730, 310, "a few SDFs per tile instead of", 13.5, INK)
    s.text(730, 330, "every capsule of the curve \u2014", 13.5, INK)
    s.text(730, 350, "that is the payoff that makes", 13.5, INK)
    s.text(730, 370, "tessellation affordable.", 13.5, INK)

    s.save("doc/bezier-tiles.svg")


def gen_rasterize():
    s = Svg(1200, 660)
    s.text(600, 42, "The rasterizer, per tile", 21, INK, "bold", "middle")
    s.card(40, 80, 700, 540, "one tile: 8x8 quads, one draw per dirty/clean pass")

    # left: the instance list
    s.text(160, 134, "instances of tile (1, 1)", 14, MUTED, "bold", "middle")
    for r in range(6):
        for c in range(6):
            fill = "#EEF2F7" if (r + c) % 2 else "#E0E7EF"
            s.rect(70 + c * 30, 146 + r * 30, 30, 30, fill=fill, stroke=PALETTE["grey"][0], sw=1)
    s.text(160, 348, "129 quads", 13, MUTED, "middle")
    s.text(160, 368, "one per command", 13, MUTED, "middle")
    s.text(160, 388, "that touches the tile", 13, MUTED, "middle")
    s.arrow(300, 236, 360, 236)

    # middle: one quad
    s.text(400, 134, "one quad (8 x 8 px)", 14, MUTED, "bold", "middle")
    for r in range(8):
        for c in range(8):
            fill = "#EEF2F7" if (r + c) % 2 else "#E0E7EF"
            s.rect(364 + c * 9, 146 + r * 9, 9, 9, fill=fill, stroke=PALETTE["grey"][0], sw=0.7)
    s.text(400, 240, "v0  v1  v2  v3", 12.5, PALETTE["orange"][0], "bold", "middle")
    s.text(400, 262, "u: 0..1, v: 0..1", 12.5, MUTED, "middle")
    s.text(400, 282, "(strip topology, 4 vertices)", 12.5, MUTED, "middle")
    s.text(440, 322, "the fragment program does", 13, MUTED, "bold", "middle")
    steps = [
        "1.  read the tile's chunk list",
        "       (6 commands per 16-byte chunk)",
        "2.  for each command: evaluate its",
        "       SDF at the fragment position",
        "3.  anti-alias at 1-px steps along",
        "       the normal direction",
        "4.  combine: min / smoothmin",
        "5.  alpha-blend into the screenshot",
        "6.  write the tile cache",
    ]
    for i, line in enumerate(steps):
        s.text(352, 344 + i * 19, line, 12.5, MUTED if i % 2 else INK)
    s.arrow(505, 512, 580, 512)
    s.card(580, 430, 150, 150, None)
    s.text(655, 462, "screenshot", 14, INK, "bold", "middle")
    s.text(655, 486, "2560 x 1440", 12, MUTED, "middle")
    s.text(655, 506, "(or the cached", 12, MUTED, "middle")
    s.text(655, 524, "tile's 8 x 8 block)", 12, MUTED, "middle")

    # right column: notes
    s.card(810, 80, 350, 300, "why no derivatives")
    notes1 = [
        "rendering happens at pixel level:",
        "one fragment per pixel, so edge",
        "quality is controlled directly",
        "and ddx/ddy are never needed",
        "",
        "one tile is covered by at most a",
        "few hundred 8-vertex quads, which",
        "a fragment shader can loop over",
        "comfortably",
    ]
    for i, line in enumerate(notes1):
        s.text(835, 128 + i * 22, line, 13, MUTED if i else PALETTE["blue"][0])
    s.card(810, 420, 350, 200, None)
    notes2 = [
        "the same pass runs on a cached",
        "tile to verify it is still valid;",
        "only the dirty pass writes the",
        "screenshot, only the clean pass",
        "writes the cache",
    ]
    for i, line in enumerate(notes2):
        s.text(835, 462 + i * 22, line, 13, MUTED if i else PALETTE["green"][0])

    s.save("doc/rasterize.svg")


if __name__ == "__main__":
    import os
    os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir))
    for fn in (gen_memory, gen_overview, gen_pipeline, gen_group_empty, gen_font_pipeline, gen_bezier_tiles, gen_rasterize):
        fn()

