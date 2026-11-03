#include "mcp.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

// The window resize (or the clamped-by-screen size) must land within this many frames of the
// request, or the shot fails with the actual framebuffer size. 120 frames (~2 s at 60 Hz) is
// generous : a programmatic setContentSize settles in one or two RESIZED events
#define MCP_SHOT_RESIZE_TIMEOUT 120

// ----------------------------------------------------------------------------------------------------------------------------
static bool mcp_buf_grow(mcp_buf* b, size_t min_size)
{
    if (b->cap >= min_size)
        return true;

    size_t cap = b->cap ? b->cap : 4096;
    while (cap < min_size)
        cap *= 2;

    char* data = (char*) realloc(b->data, cap);
    if (data == NULL)
        return false;

    b->data = data;
    b->cap = cap;
    return true;
}

static void mcp_buf_append(mcp_buf* b, const char* p, size_t n)
{
    if (mcp_buf_grow(b, b->len + n))
    {
        memcpy(b->data + b->len, p, n);
        b->len += n;
    }
}

// stbi_write_func has no return value, a failed append just truncates the output (checked later)
static void mcp_png_write(void* context, void* data, int size)
{
    mcp_buf_append((mcp_buf*) context, (const char*) data, (size_t) size);
}

// ----------------------------------------------------------------------------------------------------------------------------
static void mcp_bgra_to_rgba(uint8_t* pixels, uint32_t w, uint32_t h)
{
    for (uint32_t i = 0; i < w * h; i++)
    {
        uint8_t b = pixels[i * 4 + 0];
        pixels[i * 4 + 0] = pixels[i * 4 + 2];
        pixels[i * 4 + 2] = b;
    }
}

// ----------------------------------------------------------------------------------------------------------------------------
// Appends the base64 of [in] (NUL-terminated) to [b]
static void mcp_base64(mcp_buf* b, const uint8_t* in, size_t in_len)
{
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    size_t i = 0;
    while (i + 3 <= in_len)
    {
        uint32_t v = ((uint32_t) in[i] << 16) | ((uint32_t) in[i + 1] << 8) | in[i + 2];
        char c[4] = { table[v >> 18 & 0x3f], table[v >> 12 & 0x3f], table[v >> 6 & 0x3f], table[v & 0x3f] };
        mcp_buf_append(b, c, 4);
        i += 3;
    }

    if (i + 1 == in_len)
    {
        uint32_t v = (uint32_t) in[i] << 16;
        char c[4] = { table[v >> 18 & 0x3f], table[v >> 12 & 0x3f], '=', '=' };
        mcp_buf_append(b, c, 4);
    }
    else if (i + 2 == in_len)
    {
        uint32_t v = ((uint32_t) in[i] << 16) | ((uint32_t) in[i + 1] << 8);
        char c[4] = { table[v >> 18 & 0x3f], table[v >> 12 & 0x3f], table[v >> 6 & 0x3f], '=' };
        mcp_buf_append(b, c, 4);
    }

    mcp_buf_append(b, "", 1);   // NUL
}

// ----------------------------------------------------------------------------------------------------------------------------
static uint32_t mcp_shot_points(uint32_t px, float dpi_scale)
{
    if (dpi_scale < 1.f)
        dpi_scale = 1.f;

    uint32_t points = (uint32_t) (px / dpi_scale + 0.5f);
    return points ? points : 1;
}

// ----------------------------------------------------------------------------------------------------------------------------
void mcp_shot_init(mcp_shot* s)
{
    memset(s, 0, sizeof(*s));
}

// ----------------------------------------------------------------------------------------------------------------------------
void mcp_shot_request(mcp_shot* s, uint32_t w, uint32_t h, float dpi_scale)
{
    free(s->pixels);
    s->pixels = NULL;
    memset(&s->result, 0, sizeof(s->result));

    s->pending = true;
    s->whole = (w != 0 || h != 0);
    s->target_w = w;
    s->target_h = h;
    s->frame = 0;

    if (s->whole)
        mcp_set_window_points((int) mcp_shot_points(w, dpi_scale), (int) mcp_shot_points(h, dpi_scale));
}

// ----------------------------------------------------------------------------------------------------------------------------
static void mcp_shot_fail(mcp_shot* s, uint32_t fb_w, uint32_t fb_h)
{
    s->result.valid = true;
    s->result.ok = false;
    s->result.width = fb_w;
    s->result.height = fb_h;
    s->pending = false;
    mcp_set_window_points(MCP_WINDOW_W, MCP_WINDOW_H);
}

// ----------------------------------------------------------------------------------------------------------------------------
static bool mcp_shot_within(uint32_t a, uint32_t b, uint32_t tol)
{
    return (a > b ? a - b : b - a) <= tol;
}

// ----------------------------------------------------------------------------------------------------------------------------
bool mcp_shot_tick(mcp_shot* s, float canvas_w, float canvas_h, uint32_t fb_w, uint32_t fb_h, mcp_capture* cap)
{
    if (!s->pending)
        return false;

    if (s->whole)
    {
        if (fb_w == s->target_w && fb_h == s->target_h)
        {
            *cap = (mcp_capture) { .whole = true, .x = 0, .y = 0, .w = fb_w, .h = fb_h };
            return true;
        }

        s->frame++;
        if (s->frame < MCP_SHOT_RESIZE_TIMEOUT)
            return false;

        // the point rounding (W / dpi_scale) can lose at most a pixel
        if (mcp_shot_within(fb_w, s->target_w, 1) && mcp_shot_within(fb_h, s->target_h, 1))
        {
            *cap = (mcp_capture) { .whole = true, .x = 0, .y = 0, .w = fb_w, .h = fb_h };
            return true;
        }

        // the window never reached the requested size (clamped by the screen)
        mcp_shot_fail(s, fb_w, fb_h);
        return false;
    }

    // no size : capture the canvas rect of the current framebuffer
    mcp_transform t = mcp_fit_transform(canvas_w, canvas_h, (float) fb_w, (float) fb_h);

    uint32_t x = (uint32_t) (t.offx + 0.5f);
    uint32_t y = (uint32_t) (t.offy + 0.5f);
    uint32_t w = (uint32_t) (canvas_w * t.scale + 0.5f);
    uint32_t h = (uint32_t) (canvas_h * t.scale + 0.5f);

    // the rounding can overflow the framebuffer by a pixel (the od_set_capture_region assert)
    if (x > fb_w) x = fb_w ? fb_w - 1 : 0;
    if (y > fb_h) y = fb_h ? fb_h - 1 : 0;
    if (x + w > fb_w) w = x < fb_w ? fb_w - x : 0;
    if (y + h > fb_h) h = y < fb_h ? fb_h - y : 0;
    if (w == 0 || h == 0)
        return false;

    *cap = (mcp_capture) { .whole = false, .x = x, .y = y, .w = w, .h = h, .scale = t.scale };
    return true;
}

// ----------------------------------------------------------------------------------------------------------------------------
uint8_t* mcp_shot_prepare(mcp_shot* s, const mcp_capture* cap)
{
    s->cap = *cap;
    s->pixels = (uint8_t*) malloc((size_t) cap->w * cap->h * 4);
    return s->pixels;
}

// ----------------------------------------------------------------------------------------------------------------------------
void mcp_shot_end(mcp_shot* s)
{
    if (s->pixels == NULL)
        return;

    // the buffer was filled by od_end_frame : encode it
    uint32_t w = s->cap.w;
    uint32_t h = s->cap.h;

    mcp_bgra_to_rgba(s->pixels, w, h);

    s->png_buf.len = 0;
    s->b64_buf.len = 0;
    stbi_write_png_to_func(mcp_png_write, &s->png_buf, (int) w, (int) h, 4, s->pixels, (int) w * 4);

    // a png is always > 8 bytes : the signature (a truncated encode means a failed append)
    bool ok = s->png_buf.len > 8;
    if (ok)
    {
        s->b64_buf.len = 0;
        mcp_base64(&s->b64_buf, (const uint8_t*) s->png_buf.data, s->png_buf.len);
        ok = s->b64_buf.len > 0;
    }

    free(s->pixels);
    s->pixels = NULL;
    s->pending = false;

    s->result.valid = true;
    s->result.ok = ok;
    s->result.width = w;
    s->result.height = h;
    s->result.scale = s->cap.scale;
    s->result.png = (String_View) { ok ? s->png_buf.len : 0, ok ? s->png_buf.data : NULL };
    // the b64 buffer is NUL-terminated (mcp_base64), the view is the payload only
    s->result.b64 = (String_View) { ok ? s->b64_buf.len - 1 : 0, ok ? s->b64_buf.data : NULL };

    if (s->whole)
        mcp_set_window_points(MCP_WINDOW_W, MCP_WINDOW_H);
}
