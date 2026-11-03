#include "../tool/mcp/mcp.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); fails++; } } while (0)

// window shim stub (shot_check_stubs.c)
extern int shot_stub_last_w;
extern int shot_stub_last_h;
extern int shot_stub_calls;

static mcp_shot s;
static mcp_capture cap;

// Encodes [pixels] (w x h x 4) as if it were a capture, returns the result
static mcp_shot_result encode(uint8_t* pixels, uint32_t w, uint32_t h, float scale)
{
    mcp_capture c = { .whole = false, .x = 0, .y = 0, .w = w, .h = h, .scale = scale };
    mcp_shot_prepare(&s, &c);
    memcpy(s.pixels, pixels, (size_t) w * h * 4);
    mcp_shot_end(&s);
    return s.result;
}

// Independent base64 decoder (round-trip check of the encoder)
static bool b64_decode(const String_View in, uint8_t* out, size_t out_size)
{
    static int val[256];
    for (int i = 0; i < 256; i++)
        val[i] = -1;
    const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < 64; i++)
        val[(unsigned char) table[i]] = i;
    val['='] = 0;

    size_t o = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < in.count; i++)
    {
        int v = val[(unsigned char) in.data[i]];
        if (v < 0)
            return false;
        acc = (acc << 6) | (uint32_t) v;
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            if (o >= out_size)
                return false;
            out[o++] = (uint8_t) (acc >> bits);
        }
    }
    return bits < 6;   // trailing bits must be zero
}

int main(void)
{
    // the png / base64 outputs of a capture encode the pixels exactly
    {
        uint8_t px[16 * 4] = {0};      // the capture buffer, BGRA (onedraw's screenshot texture)
        uint8_t rgba[16 * 4] = {0};    // what the png must decode back to
        for (int i = 0; i < 16; i++)
        {
            px[i * 4 + 0] = 10; px[i * 4 + 1] = 20; px[i * 4 + 2] = 30; px[i * 4 + 3] = 255;
            rgba[i * 4 + 0] = 30; rgba[i * 4 + 1] = 20; rgba[i * 4 + 2] = 10; rgba[i * 4 + 3] = 255;
        }

        mcp_shot_result r = encode(px, 4, 4, 1.f);
        CHECK(r.valid && r.ok);
        CHECK(r.width == 4 && r.height == 4 && r.scale == 1.f);
        CHECK(r.png.count > 8);
        CHECK(r.b64.data[r.b64.count] == '\0');   // NUL-terminated, the view is the payload only

        // the png decodes back to the rgba-ized pixels
        int w = 0, h = 0, ch = 0;
        uint8_t* dec = stbi_load_from_memory((const unsigned char*) r.png.data, (int) r.png.count, &w, &h, &ch, 4);
        CHECK(dec != NULL && w == 4 && h == 4 && ch == 4);
        if (dec != NULL)
        {
            CHECK(memcmp(dec, rgba, sizeof(rgba)) == 0);
            stbi_image_free(dec);
        }

        // the base64 decodes back to the png bytes
        uint8_t* back = (uint8_t*) malloc(r.png.count + 16);
        CHECK(b64_decode(r.b64, back, r.png.count + 16) && memcmp(back, r.png.data, r.png.count) == 0);
        free(back);
    }

    // a capture with remainder 1 and 2 in base64 (padding)
    {
        uint8_t px1[1 * 4] = { 1, 2, 3, 4 };
        mcp_shot_result r1 = encode(px1, 1, 1, 1.f);
        CHECK(r1.valid && r1.ok);
        uint8_t* back = (uint8_t*) malloc(r1.png.count + 16);
        CHECK(b64_decode(r1.b64, back, r1.png.count + 16) && memcmp(back, r1.png.data, r1.png.count) == 0);
        free(back);
    }

    // sized shot : the window is resized to W/dpi x H/dpi points, the capture is due once the
    // framebuffer reports the target size, and the window is restored after the capture
    {
        mcp_shot_init(&s);

        mcp_shot_request(&s, 100, 80, 2.f);
        CHECK(shot_stub_last_w == 50 && shot_stub_last_h == 40);

        CHECK(!mcp_shot_tick(&s, 1024, 768, 1280, 720, &cap));       // still previewing
        CHECK(!mcp_shot_tick(&s, 1024, 768, 1280, 720, &cap));
        CHECK(mcp_shot_tick(&s, 1024, 768, 100, 80, &cap));          // the resize landed
        CHECK(cap.whole && cap.w == 100 && cap.h == 80 && cap.x == 0 && cap.y == 0);

        uint8_t* buf = mcp_shot_prepare(&s, &cap);
        CHECK(buf != NULL && s.pixels == buf);
        CHECK(shot_stub_last_w == 50);                               // no restore before the capture
        mcp_shot_end(&s);
        CHECK(shot_stub_last_w == MCP_WINDOW_W && shot_stub_last_h == MCP_WINDOW_H);
        CHECK(!s.pending);
    }

    // odd size on a 2x screen : the requested points (51 x 41) render a framebuffer 1 px off
    // the requested odd size, the ±1 tolerance still captures it
    {
        mcp_shot_init(&s);
        mcp_shot_request(&s, 101, 81, 2.f);

        for (int i = 0; i < 200; i++)
        {
            if (mcp_shot_tick(&s, 1024, 768, 102, 82, &cap))
                break;
        }
        CHECK(cap.w == 102 && cap.h == 82);
        mcp_shot_prepare(&s, &cap);
        mcp_shot_end(&s);
        CHECK(s.result.valid && s.result.ok);
        CHECK(s.result.width == 102 && s.result.height == 82);
    }

    // the window never reaches the requested size (clamped by the screen) : error with the
    // actual framebuffer size after the deadline, and the window is restored
    {
        mcp_shot_init(&s);
        mcp_shot_request(&s, 4000, 3000, 2.f);                       // 2000 x 1500 points, doesn't fit

        bool failed = false;
        for (int i = 0; i < 200; i++)
        {
            mcp_shot_tick(&s, 1024, 768, 1024, 768, &cap);          // clamped by the screen
            if (s.result.valid)
            {
                failed = true;
                break;
            }
        }
        CHECK(failed);
        CHECK(!s.result.ok);
        CHECK(s.result.width == 1024 && s.result.height == 768);
        CHECK(!s.pending);
        CHECK(shot_stub_last_w == MCP_WINDOW_W && shot_stub_last_h == MCP_WINDOW_H);
    }

    // the sizeless shot captures the canvas rect of the current framebuffer, no window resize
    {
        mcp_shot_init(&s);
        int calls = shot_stub_calls;
        mcp_shot_request(&s, 0, 0, 2.f);
        CHECK(shot_stub_calls == calls);                             // no resize

        CHECK(mcp_shot_tick(&s, 1024, 768, 1280, 720, &cap));
        // scale = min(1280/1024, 720/768) = 0.9375, canvas 960 x 720 centered
        CHECK(!cap.whole);
        CHECK(cap.x == 160 && cap.y == 0 && cap.w == 960 && cap.h == 720);
        CHECK(cap.scale == 720.f / 768.f);
    }

    // the canvas rect is clamped inside the framebuffer (the od_set_capture_region assert)
    {
        mcp_shot_init(&s);
        mcp_shot_request(&s, 0, 0, 2.f);

        // an odd canvas over a framebuffer that the rounding would overflow by a pixel
        CHECK(mcp_shot_tick(&s, 1023, 767, 1280, 720, &cap));
        CHECK(cap.x + cap.w <= 1280 && cap.y + cap.h <= 720);
        CHECK(cap.w > 0 && cap.h > 0);
    }

    // a request replaces the pending shot
    {
        mcp_shot_init(&s);
        mcp_shot_request(&s, 0, 0, 2.f);
        CHECK(mcp_shot_tick(&s, 1024, 768, 1280, 720, &cap));      // the sizeless shot is due this frame
        CHECK(!cap.whole);
        mcp_shot_request(&s, 100, 80, 2.f);                        // replaced by a sized shot
        CHECK(shot_stub_last_w == 50 && shot_stub_last_h == 40);   // the new request resizes the window
        CHECK(!mcp_shot_tick(&s, 1024, 768, 1280, 720, &cap));    // it waits for the resize to land
        mcp_shot_request(&s, 0, 0, 2.f);                           // ...and is itself replaced
        CHECK(mcp_shot_tick(&s, 1024, 768, 1280, 720, &cap));
        CHECK(!cap.whole && cap.w == 960 && cap.h == 720);
    }

    printf(fails ? "shot checks: %d FAILURES\n" : "shot checks: all ok\n", fails);
    return fails ? 1 : 0;
}
