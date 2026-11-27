#include "lean_ui.h"
#include <assert.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <math.h>

#define MAX_WINDOWS (16)
#define MAX_PROGRESS (16)
#define STRING_BUFFER_SIZE (2048)
#define ANIMATION_DURATION (.2f)
#define HOVER_DURATION (.1f)

//-----------------------------------------------------------------------------------------------------------------------------
// Structures
//-----------------------------------------------------------------------------------------------------------------------------

typedef struct {float x, y;} ui_vec2;

typedef struct
{
    const char* name;
    uint32_t id;
    ui_vec2 pos;
    float width, height;
    float min_width, min_height;
    float scroll, scroll_target;
    float content_height;
    float scroll_hover;
    bool scrollbar_visible;
    uint32_t options;
    bool closed;
} ui_window;

typedef struct
{
    uint32_t window_border;
    uint32_t window_bg;
    uint32_t title_bg;
    uint32_t title_text;
    uint32_t widget_bg;
    uint32_t widget_hover;
    uint32_t widget_active;
    uint32_t text;
    uint32_t accent;
    uint32_t value_bg;
    uint32_t value_text;
    uint32_t separator;
    uint32_t resize_handle;
    uint32_t scrollbar_idle;
    uint32_t scrollbar_hover;
} ui_colors;

typedef struct
{
    float value_key0, value_key1;
    uint32_t color_key0, color_key1;
    bool key0_to_key1;
    float t;
    void* widget;
} ui_animation;

typedef struct 
{
    void* widget;
    float t;
} ui_hover;

typedef struct
{
    uint32_t id;
    float animated;
} ui_progress_state;


struct ui_context
{
    ui_vec2 mouse_pos;
    enum ui_button_state mouse_button;
    bool mouse_down;
    bool mouse_doubleclick;
    float doubleclick_timer;
    float mouse_wheel;
    float delta_time;
    ui_window windows[MAX_WINDOWS];
    uint32_t num_windows;
    ui_window* current_window;
    ui_window* resizing_window;
    void* dragging_object;
    ui_animation animation;
    ui_hover hover;
    ui_hover thumb_hover;
    ui_progress_state progress_states[MAX_PROGRESS];
    uint32_t num_progress_states;
    ui_vec2 dragging_offset;
    ui_rect layout;
    ui_rect content_rect;
    float font_height;
    float row_height;
    float padding;
    float corner;
    ui_colors colors;
    ui_renderer_fnc_t renderer;
    char string_buffer[STRING_BUFFER_SIZE];
};

//-----------------------------------------------------------------------------------------------------------------------------
// inline functions
//-----------------------------------------------------------------------------------------------------------------------------

static inline ui_vec2 ui_vec2_sub(ui_vec2 a, ui_vec2 b) {return (ui_vec2){a.x - b.x, a.y - b.y};}
static inline ui_vec2 ui_vec2_add(ui_vec2 a, ui_vec2 b) {return (ui_vec2){a.x + b.x, a.y + b.y};}
static inline void expand_rect(ui_rect* rect, float amount) {rect->x-=amount; rect->y-=amount; rect->width+=amount*2.f; rect->height+=amount*2.f;}
static inline float lerp_float(float a, float b, float t) {return fmaf(b - a, t, a);}
static inline float clamp_float(float min_value, float max_value, float f) {return fminf(max_value, fmaxf(min_value, f));}
static inline float ease_in_quad(float x) {return x * x;}
static inline float ease_in_cubic(float x) {return x * x * x;}
static inline float ease_impulse(float x) {return ease_in_cubic(sinf(x * 3.14159265f));}
static inline float ease_in_expo(float x){return (x == 0.f) ? 0.f : powf(2.f, 10.f * x - 10.f);}

//-----------------------------------------------------------------------------------------------------------------------------
static inline bool in_rect(const ui_rect *rect, ui_vec2 pos)
{
    return ((pos.x>=rect->x) && (pos.x<=rect->x+rect->width) && (pos.y>=rect->y) && (pos.y<=rect->y+rect->height));
}

//-----------------------------------------------------------------------------------------------------------------------------
static inline bool ui_hovered(const ui_context* ctx, const ui_rect* r)
{
    // widgets scrolled under the title bar or below the window must not catch clicks
    return in_rect(r, ctx->mouse_pos) && in_rect(&ctx->content_rect, ctx->mouse_pos);
}

//-----------------------------------------------------------------------------------------------------------------------------
static inline float ease_out_back(float x)
{
    const float c1 = 0.8f;
    const float c3 = c1 + 1.f;
    return 1.f + c3 * ease_in_cubic(x - 1.f) + c1 * ease_in_quad(x - 1.f);
}

//-----------------------------------------------------------------------------------------------------------------------------
static inline uint32_t ui_hash(const void *data, size_t length)
{
    uint32_t hash = 0x811c9dc5;
    const uint8_t* p = (uint8_t*) data;

    for(size_t i=0; i<length; ++i)
        hash = (hash ^ *p++) * 0x01000193;

    return hash;
}

//-----------------------------------------------------------------------------------------------------------------------------
static inline uint32_t lerp_color(uint32_t a, uint32_t b, float t)
{
    int tt = (int)(t * 256.f);
    int oneminust = 256 - tt;

    uint32_t A = (((a >> 24) & 0xFF) * oneminust + ((b >> 24) & 0xFF) * tt) >> 8;
    uint32_t B = (((a >> 16) & 0xFF) * oneminust + ((b >> 16) & 0xFF) * tt) >> 8;
    uint32_t G = (((a >> 8)  & 0xFF) * oneminust + ((b >> 8)  & 0xFF) * tt) >> 8;
    uint32_t R = (((a >> 0)  & 0xFF) * oneminust + ((b >> 0)  & 0xFF) * tt) >> 8;

    return (A << 24) | (B << 16) | (G << 8) | R;
}

//-----------------------------------------------------------------------------------------------------------------------------
static inline void draw_align_string_view(ui_context* ctx, const ui_rect* rect, const char* text, size_t length,
                                          uint32_t srgb_color, enum ui_text_alignment alignment)
{
    float x = rect->x;
    if (alignment != align_left)
    {
        float w = ctx->renderer.text_width(text, length, ctx->renderer.user);
        x += (alignment == align_right) ? rect->width - w : (rect->width - w) * .5f;
    }
    ctx->renderer.draw_text(x, rect->y, text, length, srgb_color, ctx->renderer.user);
}

//-----------------------------------------------------------------------------------------------------------------------------
static inline void draw_align_text(ui_context* ctx, const ui_rect* rect, const char* text, uint32_t srgb_color, enum ui_text_alignment alignment)
{
    draw_align_string_view(ctx, rect, text, strlen(text), srgb_color, alignment);
}

//-----------------------------------------------------------------------------------------------------------------------------
static inline float text_width(ui_context* ctx, const char* text) 
{
    return ctx->renderer.text_width(text, strlen(text), ctx->renderer.user);
}

//-----------------------------------------------------------------------------------------------------------------------------
static inline void draw_disc(ui_context* ctx, float x, float y, float radius, uint32_t srgb_color)
{
    ctx->renderer.draw_box(x - radius, y - radius, radius*2.f, radius*2.f, radius, srgb_color, ctx->renderer.user);
}

//-----------------------------------------------------------------------------------------------------------------------------
// UI functions
//-----------------------------------------------------------------------------------------------------------------------------

// one row per role, in ui_colors field order : {lightness in light mode, lightness in dark mode, chroma at tint 1}
static const float palette_recipe[][3] =
{
    {.86f, .36f, .031f},  // window_border
    {.96f, .27f, .012f},  // window_bg
    {.90f, .32f, .030f},  // title_bg
    {.33f, .93f, .030f},  // title_text
    {.93f, .33f, .021f},  // widget_bg
    {.89f, .37f, .029f},  // widget_hover
    {.85f, .42f, .038f},  // widget_active
    {.41f, .88f, .037f},  // text
    {.70f, .56f, 0.f  },  // accent (chroma computed separately)
    {.92f, .24f, .018f},  // value_bg
    {.26f, .96f, .018f}   // value_text
};
_Static_assert(sizeof(ui_colors) == 15*sizeof(uint32_t), "palette_recipe indexes ui_colors as an array of uint32_t");

//-----------------------------------------------------------------------------------------------------------------------------
static uint32_t oklch_to_srgb(float L, float C, float hue_deg)
{
    float h = hue_deg * (3.14159265f/180.f), a = C*cosf(h), b = C*sinf(h);
    float l = L + .3963377774f*a + .2158037573f*b; l = l*l*l;
    float m = L - .1055613458f*a - .0638541728f*b; m = m*m*m;
    float s = L - .0894841775f*a - 1.2914855480f*b; s = s*s*s;
    float rgb[3] = { 4.0767416621f*l - 3.3077115913f*m + .2309699292f*s,
                    -1.2684380046f*l + 2.6097574011f*m - .3413193965f*s,
                    -.0041960863f*l - .7034186147f*m + 1.7076147010f*s};
    uint32_t out = 0xFF000000;
    for (int i = 0; i < 3; ++i)
    {
        float x = fminf(fmaxf(rgb[i], 0.f), 1.f);   // clamp out-of-gamut colors
        x = (x <= .0031308f) ? 12.92f*x : 1.055f*powf(x, 1.f/2.4f) - .055f;
        out |= (uint32_t)(x*255.f + .5f) << (i*8);  // R in the low byte
    }
    return out;
}

//-----------------------------------------------------------------------------------------------------------------------------
void ui_set_palette(ui_context* ctx, float hue, float accent_hue, float tint, bool dark)
{
    uint32_t* c = (uint32_t*)&ctx->colors;  // ui_colors holds only uint32_t, in recipe order
    for (size_t i = 0; i < sizeof(palette_recipe)/sizeof(palette_recipe[0]); ++i)
        c[i] = oklch_to_srgb(palette_recipe[i][dark], palette_recipe[i][2]*tint, hue);
    ctx->colors.accent = oklch_to_srgb(palette_recipe[8][dark], .05f + .056f*tint, accent_hue);
    ctx->colors.separator = (ctx->colors.text & 0x00FFFFFF) | 0x40000000;
    ctx->colors.resize_handle = ctx->colors.text;
    ctx->colors.scrollbar_idle = (ctx->colors.text & 0x00FFFFFF) | 0x80000000;
    ctx->colors.scrollbar_hover = (ctx->colors.text & 0x00FFFFFF) | 0xA0000000;
}

//-----------------------------------------------------------------------------------------------------------------------------
size_t ui_min_memory_size(void)
{
    return sizeof(ui_context);
}

//-----------------------------------------------------------------------------------------------------------------------------
ui_context* ui_init(const ui_def* def)
{
    assert(def->renderer_callbacks.draw_box && def->renderer_callbacks.draw_text &&
           def->renderer_callbacks.set_clip_rect && def->renderer_callbacks.text_width &&
           def->renderer_callbacks.draw_line);
    
    assert(((uintptr_t)def->preallocated_buffer)%sizeof(uintptr_t) == 0);

    ui_context* ctx = (ui_context*)def->preallocated_buffer;
    *ctx = (ui_context)
    {
        .mouse_button = button_idle,
        .font_height = def->font_height,
        .renderer = def->renderer_callbacks,
        .padding = fmaxf(def->font_height/4.f, 2.f),
        .corner = fmaxf(def->font_height/2.f, 2.f)
    };
    ui_set_palette(ctx, 250.f, 250.f, .7f, false);
    ctx->row_height = ctx->font_height * 1.5f;
    return ctx;
}

//-----------------------------------------------------------------------------------------------------------------------------
void ui_update_mouse_pos(ui_context* ctx, float x, float y)
{
    ctx->mouse_pos = (ui_vec2) {x, y};
}

//-----------------------------------------------------------------------------------------------------------------------------
void ui_update_mouse_button(ui_context* ctx, enum ui_button_state button)
{
    ctx->mouse_button = button;

    if (button == button_pressed)
    {
        ctx->mouse_down = true;
        ctx->mouse_doubleclick = (ctx->doubleclick_timer < 0.25f);
        ctx->doubleclick_timer = 0.f;
    }
    else
    {
        ctx->mouse_down = false;
        ctx->mouse_doubleclick = false;
    }
}

//-----------------------------------------------------------------------------------------------------------------------------
void ui_update_mouse_wheel(ui_context* ctx, float delta)
{
    ctx->mouse_wheel += delta;
}

//-----------------------------------------------------------------------------------------------------------------------------
void ui_begin_frame(ui_context* ctx, float delta_time)
{
    ctx->current_window = NULL;
    ctx->delta_time = delta_time;

    // a window/widget grab is only valid while the button is held down
    if (!ctx->mouse_down)
    {
        ctx->resizing_window = NULL;
        ctx->dragging_object = NULL;
    }

    ctx->animation.t = fminf(1.f, ctx->animation.t + delta_time/ANIMATION_DURATION);
    ctx->hover.t = fminf(1.f, ctx->hover.t + delta_time/HOVER_DURATION);
    ctx->doubleclick_timer += delta_time;
}

//-----------------------------------------------------------------------------------------------------------------------------
void ui_begin_window(ui_context* ctx, const char* name, float x, float y, float width, float height, uint32_t options)
{
    assert(ctx->current_window == NULL);

    uint32_t id = ui_hash(name, strlen(name));

    // already created?
    for(uint32_t i=0; i<ctx->num_windows; ++i)
        if (ctx->windows[i].id == id)
            ctx->current_window = &ctx->windows[i];

    // not found, create one
    if (ctx->current_window == NULL)
    {
        assert(ctx->num_windows < MAX_WINDOWS);
        ctx->current_window = &ctx->windows[ctx->num_windows++];
        *ctx->current_window = (ui_window)
        {
            .name = name,
            .id = id,
            .pos = {.x = x, .y = y},
            .min_width = text_width(ctx, name) + ctx->padding * 2.f,
            .min_height = (options & window_no_titlebar) ? ctx->row_height * 2.f : ctx->row_height * 3.f,
            .width = width,
            .height = height,
            .options = options,
            .closed = false
        };
    }

    ui_window* w = ctx->current_window;

    // resize
    ui_rect handle_rect = {w->pos.x + w->width - ctx->corner, w->pos.y + w->height - ctx->corner, ctx->corner, ctx->corner};
    if (ctx->mouse_button == button_pressed && in_rect(&handle_rect, ctx->mouse_pos) && (w->options&window_resizable) && ctx->resizing_window == NULL)
    {
        ctx->resizing_window = w;
        ctx->dragging_offset = ui_vec2_sub(ui_vec2_add(w->pos, (ui_vec2) {w->width, w->height}), ctx->mouse_pos);
    }

    if (ctx->mouse_down && ctx->resizing_window == w)
    {
        w->width = fmaxf(ctx->mouse_pos.x + ctx->dragging_offset.x - w->pos.x, w->min_width);
        w->height = fmaxf(ctx->mouse_pos.y + ctx->dragging_offset.y - w->pos.y, w->min_height);
    }

    bool titlebar = !(w->options & window_no_titlebar);
    bool border = !(w->options & window_no_border);
    float inset = border ? ctx->padding : 0.f;
    float title_height = ctx->font_height + ctx->padding;

    ui_rect title_rect = {w->pos.x + inset, w->pos.y + inset, w->width - inset * 2.f, title_height};

    // move the window if click on the title bar : a window without a title bar has no drag handle
    if (titlebar && ctx->mouse_button == button_pressed && in_rect(&title_rect, ctx->mouse_pos) &&
        !(w->options&window_pinned) && ctx->resizing_window != w && ctx->dragging_object == NULL)
    {
        ctx->dragging_object = w;
        ctx->dragging_offset = ui_vec2_sub(ctx->mouse_pos, w->pos);
    }

    if (ctx->mouse_down && ctx->dragging_object == w)
        w->pos = ui_vec2_sub(ctx->mouse_pos, ctx->dragging_offset);

    // border frame, or the plain card a borderless window is made of
    if (border)
        ctx->renderer.draw_box(w->pos.x, w->pos.y, w->width, w->height, ctx->corner, ctx->colors.window_border, ctx->renderer.user);
    else
        ctx->renderer.draw_box(w->pos.x, w->pos.y, w->width, w->height, ctx->corner, ctx->colors.window_bg, ctx->renderer.user);

    // title bg
    if (titlebar)
        ctx->renderer.draw_box(title_rect.x, title_rect.y, title_rect.width, title_rect.height, ctx->corner, ctx->colors.title_bg, ctx->renderer.user);

    // vertical scrolling : content_height comes from the previous frame
    float top = w->pos.y + inset + (titlebar ? title_height + ctx->padding : ctx->padding * 2.f);
    float visible_h = w->pos.y + w->height - ctx->padding - top;
    float max_scroll = fmaxf(0.f, w->content_height - visible_h);
    w->scrollbar_visible = (max_scroll > 0.f);

    // the layout leaves the scrollbar's max width (1.5 * padding) free at the right edge so content never ends up
    // under the bar, but only when the bar actually shows
    ctx->layout = (ui_rect)
    {
        .x = w->pos.x + ctx->padding,
        .y = top,
        .width = w->width - (w->scrollbar_visible ? ctx->padding*3.5f : ctx->padding*2.f),
        .height = ctx->row_height
    };
    ctx->content_rect = (ui_rect){ctx->layout.x, top, ctx->layout.width, visible_h};

    ui_rect win = {w->pos.x, w->pos.y, w->width, w->height};
    if (in_rect(&win, ctx->mouse_pos))
        w->scroll_target -= ctx->mouse_wheel * ctx->row_height * 2.f;

    // scrollbar in the window's right edge : the hit rect matches the visible bar, which eases between an idle
    // width (1 * padding) and a hover width (1.5 * padding), and grows as soon as the cursor enters the full-height
    // strip of its widest size
    ui_rect thumb = {0};
    float bar_w = 0.f;
    if (max_scroll > 0.f)
    {
        float thumb_h = fmaxf(visible_h * visible_h / w->content_height, ctx->row_height * .5f);
        float bar_right = w->pos.x + w->width - ctx->padding;
        bar_w = lerp_float(ctx->padding, ctx->padding * 1.5f, w->scroll_hover);
        thumb = (ui_rect){bar_right - bar_w, top + (w->scroll / max_scroll) * (visible_h - thumb_h), bar_w, thumb_h};
        ui_rect hover_rect = {bar_right - ctx->padding * 1.5f, top, ctx->padding * 1.5f, visible_h};
        bool hovered = in_rect(&hover_rect, ctx->mouse_pos);
        if (ctx->mouse_button == button_pressed && in_rect(&thumb, ctx->mouse_pos) && ctx->dragging_object == NULL && ctx->resizing_window != w)
        {
            ctx->dragging_object = &w->scroll;
            ctx->dragging_offset.y = ctx->mouse_pos.y - thumb.y;
        }

        if (ctx->mouse_down && ctx->dragging_object == &w->scroll)
            w->scroll = w->scroll_target = clamp_float(0.f, max_scroll, (ctx->mouse_pos.y - ctx->dragging_offset.y - top) / (visible_h - thumb_h) * max_scroll);

        // hover fade in and out, framerate independent
        float hover_goal = (hovered || ctx->dragging_object == &w->scroll) ? 1.f : 0.f;
        w->scroll_hover = lerp_float(w->scroll_hover, hover_goal, 1.f - expf(-15.f * ctx->delta_time));
    }

    w->scroll_target = clamp_float(0.f, max_scroll, w->scroll_target);
    w->scroll = lerp_float(w->scroll, w->scroll_target, 1.f - expf(-20.f * ctx->delta_time));

    // background, full inner width : the scrollbar's reserved strip stays part of the window background,
    // a borderless window already drew its background as the card
    if (border)
        ctx->renderer.draw_box(ctx->layout.x, ctx->layout.y, w->width - ctx->padding*2.f,
                               w->pos.y + w->height - ctx->padding - ctx->layout.y, 0, ctx->colors.window_bg, ctx->renderer.user);

    // scrollbar bar, anchored to the window's right edge so it grows towards the left
    if (max_scroll > 0.f)
    {
        uint32_t bar_c = lerp_color(ctx->colors.scrollbar_idle, ctx->colors.scrollbar_hover, w->scroll_hover);
        ctx->renderer.draw_box(thumb.x, thumb.y, thumb.width, thumb.height, bar_w * .5f, bar_c, ctx->renderer.user);
    }

    ctx->layout.x += ctx->padding;
    ctx->layout.width -= 2.f * ctx->padding;

    // title, vertically centered in the strip : draw_text is fed the top of a row_height-tall
    // row (like every other row of text), so center that row in the strip
    if (titlebar)
    {
        ui_rect text_rect = title_rect;
        text_rect.y += (text_rect.height - ctx->row_height) * .5f;
        draw_align_text(ctx, &text_rect, w->name, ctx->colors.title_text, align_center);
    }

    // resize handle : five dots along the ring's corner curve, full sweep from edge to edge
    if (w->options&window_resizable)
    {
        float r = ctx->corner * .1f;
        float rho = ctx->corner - ctx->padding * .5f;   // ring band centerline radius
        for (int i = 0; i < 5; ++i)
        {
            float a = 22.5f * i * 3.14159265f / 180.f;
            float x = handle_rect.x + rho * cosf(a) - r;
            float y = handle_rect.y + rho * sinf(a) - r;
            ctx->renderer.draw_box(x, y, r * 2.f, r * 2.f, r, ctx->colors.resize_handle, ctx->renderer.user);
        }
    }

    // clip rect, set from the unshifted layout : the clip stays put while the content scrolls in it
    uint16_t clip_minx = (uint16_t) fmaxf(ctx->layout.x, 0.f);
    uint16_t clip_miny = (uint16_t) fmaxf(ctx->layout.y, 0.f);
    uint16_t clip_maxx = (uint16_t) (ctx->layout.x + ctx->layout.width + .5f);
    uint16_t clip_maxy = (uint16_t) (w->pos.y + w->height - ctx->padding + .5f);
    ctx->renderer.set_clip_rect(clip_minx, clip_miny, clip_maxx, clip_maxy, ctx->renderer.user);

    ctx->layout.y -= w->scroll;
}

//-----------------------------------------------------------------------------------------------------------------------------
void ui_text(ui_context* ctx, enum ui_text_alignment alignment, const char* string, ...)
{
    assert(ctx->current_window != NULL);

    va_list args;
    va_start(args, string);
    vsnprintf(ctx->string_buffer, STRING_BUFFER_SIZE, string, args);
    va_end(args);

    draw_align_text(ctx, &ctx->layout, ctx->string_buffer, ctx->colors.text, alignment);
}

//-----------------------------------------------------------------------------------------------------------------------------
void ui_paragraph(ui_context* ctx, const char* text, size_t length)
{
    assert(ctx->current_window != NULL);

    const char* end = text + length;
    while (text < end)
    {
        size_t fit = 0;                                  // length of the longest line that fits
        for (size_t i = 0; ; ++i)
            if (text + i == end || text[i] == ' ' || text[i] == '\n')
            {
                if (fit > 0 && ctx->renderer.text_width(text, i, ctx->renderer.user) > ctx->layout.width)
                    break;                               // keep the previous break point
                fit = i;
                if (text + i == end || text[i] == '\n')
                    break;                               // end of text or hard break
            }

        draw_align_string_view(ctx, &ctx->layout, text, fit, ctx->colors.text, align_left);
        ui_newline(ctx);
        text += fit;
        if (text < end) ++text;                          // skip the space or '\n' we broke on
    }
}

//-----------------------------------------------------------------------------------------------------------------------------
void ui_newline(ui_context* ctx)
{
    assert(ctx->current_window != NULL);
    ui_window* w = ctx->current_window;
    ctx->layout.x = w->pos.x + ctx->padding * 2.f;
    ctx->layout.y += ctx->layout.height;
    ctx->layout.width = w->width - (w->scrollbar_visible ? ctx->padding * 5.5f : ctx->padding * 4.f);
    ctx->layout.height = ctx->row_height;
}

//-----------------------------------------------------------------------------------------------------------------------------
void ui_separator(ui_context* ctx)
{
    assert(ctx->current_window != NULL);

    float y = ctx->layout.y + .5f * ctx->layout.height;
    ctx->renderer.draw_box(ctx->layout.x, y, ctx->layout.width, 1.f, 1.f, ctx->colors.separator, ctx->renderer.user);
    ui_newline(ctx);
}

//-----------------------------------------------------------------------------------------------------------------------------
void ui_value(ui_context* ctx, const char* label, const char* fmt, ...)
{
    assert(ctx->current_window != NULL);

    va_list args;
    va_start(args, fmt);
    vsnprintf(ctx->string_buffer, STRING_BUFFER_SIZE, fmt, args);
    va_end(args);

    ui_rect value_rect = {ctx->layout.x + ctx->layout.width*.5f, ctx->layout.y, ctx->layout.width*.5f, ctx->layout.height};

    draw_align_text(ctx, &ctx->layout, label, ctx->colors.text, align_left);
    ctx->renderer.draw_box(value_rect.x, value_rect.y, value_rect.width, value_rect.height, 0, ctx->colors.value_bg, ctx->renderer.user);
    ctx->renderer.draw_box(value_rect.x-0.5f, value_rect.y+ctx->padding, 1.f, value_rect.height-ctx->padding*2, 0, ctx->colors.separator, ctx->renderer.user);

    draw_align_text(ctx, &value_rect, ctx->string_buffer, ctx->colors.value_text, align_right);
    ui_newline(ctx);
}

//-----------------------------------------------------------------------------------------------------------------------------
void ui_toggle(ui_context* ctx, const char* label, bool* value)
{
    draw_align_text(ctx, &ctx->layout, label, ctx->colors.text, align_left);

    ui_rect track_rect = 
    {
        .x = ctx->layout.x + ctx->layout.width - ctx->font_height*2.f - ctx->padding,
        .y = ctx->layout.y+ctx->padding,
        .width = ctx->font_height*2.f,
        .height = ctx->font_height
    };

    if (ui_hovered(ctx, &track_rect) && ctx->mouse_button == button_pressed)
    {
        *value = !(*value);

        // setup tweening animation, animate thumb position and track color
        ctx->animation = (ui_animation)
        {
            .value_key0 = track_rect.x + ctx->font_height - 2.f,
            .value_key1 = track_rect.x + 2.f,
            .color_key0 = ctx->colors.accent,
            .color_key1 = ctx->colors.separator,
            .key0_to_key1 = !(*value),
            .widget = value
        };
    }

    ui_rect thumb_rect =
    {
        .x = (*value) ? track_rect.x + ctx->font_height - 2.f : track_rect.x + 2.f,
        .y = track_rect.y + 2.f,
        .width = ctx->font_height - 4.f,
        .height = ctx->font_height - 4.f
    };

    uint32_t track_color;
    if (ctx->animation.widget == value)
    {
        float t = (ctx->animation.key0_to_key1) ? ctx->animation.t : 1.f - ctx->animation.t;
        thumb_rect.x = lerp_float(ctx->animation.value_key0, ctx->animation.value_key1, t);
        track_color = lerp_color(ctx->animation.color_key0, ctx->animation.color_key1, t);
    }
    else
        track_color = (*value) ? ctx->colors.accent : ctx->colors.separator;

    
    ctx->renderer.draw_box(track_rect.x, track_rect.y, track_rect.width, track_rect.height,
                           track_rect.height*.5f, track_color, ctx->renderer.user);

    if (ui_hovered(ctx, &track_rect))
        expand_rect(&thumb_rect, 1.f);
    
    ctx->renderer.draw_box(thumb_rect.x, thumb_rect.y, thumb_rect.width, thumb_rect.height,
                            thumb_rect.height*.5f, ctx->colors.text, ctx->renderer.user);

    ui_newline(ctx);
}

//-----------------------------------------------------------------------------------------------------------------------------
void ui_segmented(ui_context* ctx, const char** entries, uint32_t num_entries, uint32_t* selected)
{
    assert(num_entries > 0);
    ui_rect seg_rect = 
    {
        .x = ctx->layout.x,
        .y = ctx->layout.y,
        .width = ctx->layout.width / (float)num_entries,
        .height = ctx->font_height
    };

    ctx->renderer.draw_box(seg_rect.x, seg_rect.y + ctx->padding, ctx->layout.width, seg_rect.height,
                           ctx->corner, ctx->colors.widget_bg, ctx->renderer.user);
    
    if (*selected < num_entries)
    {
        float x = (ctx->animation.widget == selected) ? 
                    lerp_float(ctx->animation.value_key0, ctx->animation.value_key1, ease_out_back(ctx->animation.t)) : 
                    seg_rect.x + seg_rect.width * (*selected);

        ctx->renderer.draw_box(x + ctx->padding, seg_rect.y + ctx->padding, seg_rect.width - 2.f * ctx->padding,
                               seg_rect.height, ctx->padding, ctx->colors.accent, ctx->renderer.user);
    }

    for(uint32_t i=0; i<num_entries; ++i)
    {
        ui_rect button_rect = seg_rect; button_rect.y += ctx->padding;
        if (ui_hovered(ctx, &button_rect))
        {
            if (ctx->mouse_button == button_pressed)
            {
                ctx->animation = (ui_animation)
                {
                    .value_key0 = ctx->layout.x + seg_rect.width * (*selected),
                    .value_key1 = ctx->layout.x + seg_rect.width * i,
                    .widget = selected
                };

                *selected = i;
            }
            else if (i != *selected)
            {
                ctx->renderer.draw_box(seg_rect.x, seg_rect.y + ctx->padding, seg_rect.width, seg_rect.height, 
                                      ctx->padding, ctx->colors.widget_hover, ctx->renderer.user);
            }
        }

        draw_align_text(ctx, &seg_rect, entries[i], ctx->colors.text, align_center);

        if (i>0) // separator
            ctx->renderer.draw_box(seg_rect.x-.5f, seg_rect.y+ctx->padding*2.f, 1.f, seg_rect.height-2.f*ctx->padding,
                                   0, ctx->colors.separator, ctx->renderer.user);

        seg_rect.x += seg_rect.width;
    }
    ui_newline(ctx);
}


//-----------------------------------------------------------------------------------------------------------------------------
void ui_slider(ui_context* ctx, const char* label, float min_value, float max_value, float step, float* value, const char* fmt)
{
    assert(max_value>min_value);

    // always clamp in case the user change the value
    *value = clamp_float(min_value, max_value, *value);

    // first row is just label + value
    snprintf(ctx->string_buffer, STRING_BUFFER_SIZE, fmt, *value);
    draw_align_text(ctx, &ctx->layout, label, ctx->colors.text, align_left);
    draw_align_text(ctx, &ctx->layout, ctx->string_buffer, ctx->colors.text, align_right);
    ui_newline(ctx);

    float center_y = ctx->layout.y + .5f * ctx->layout.height;

    // track
    ui_rect track_rect = 
    { 
        .x = ctx->layout.x + .1f * ctx->layout.width,
        .y = center_y - ctx->padding*.5f,
        .width = .8f * ctx->layout.width,
        .height = ctx->padding
    };

    // thumb geometry, a disc centered on the thumb position
    float norm_value = (*value - min_value) / (max_value - min_value);
    float thumb_x = norm_value * track_rect.width + track_rect.x;
    float thumb_size = ctx->font_height - 4.f;
    float half_size = thumb_size * .5f;

    ui_rect thumb_rect =
    {
        .x = thumb_x - half_size,
        .y = center_y - half_size,
        .width = thumb_size,
        .height = thumb_size
    };

    const bool track_hovered = ui_hovered(ctx, &track_rect);
    const bool thumb_hovered = ui_hovered(ctx, &thumb_rect);

    // hover grow animation, eased in and out
    if (thumb_hovered)
    {
        if (ctx->thumb_hover.widget != value)
            ctx->thumb_hover = (ui_hover) {.widget = value, .t = 0.f};
        ctx->thumb_hover.t = fminf(1.f, ctx->thumb_hover.t + ctx->delta_time / HOVER_DURATION);
    }
    else if (ctx->thumb_hover.widget == value)
    {
        ctx->thumb_hover.t -= ctx->delta_time / HOVER_DURATION;
        if (ctx->thumb_hover.t <= 0.f)
            ctx->thumb_hover = (ui_hover) {.widget = NULL, .t = 0.f};
    }
    const float hover_grow = (ctx->thumb_hover.widget == value) ? ctx->thumb_hover.t : 0.f;

    float grown_size = thumb_size + 4.f * hover_grow;
    float grown_half = grown_size * .5f;

    // track change color on mouse-over
    uint32_t track_color = (track_hovered || (ctx->dragging_object == value)) ? ctx->colors.widget_hover : ctx->colors.widget_bg;
    ctx->renderer.draw_box(track_rect.x, track_rect.y, track_rect.width, track_rect.height,
                           track_rect.height*.5f, track_color, ctx->renderer.user);

    // thumb mouse-over and dragging
    if (thumb_hovered)
    {
        if (ctx->mouse_button == button_pressed)
        {
            ctx->dragging_object = value;
            ctx->dragging_offset.x = ctx->mouse_pos.x - thumb_x;

            if (ctx->animation.widget == value)
                ctx->animation.widget = NULL;
        }
    }
    else if (track_hovered && ctx->mouse_button == button_pressed)
    {
        // click on track makes the thumb glide to the cursor
        ctx->animation = (ui_animation)
        {
            .widget = value,
            .value_key0 = thumb_x,
            .value_key1 = ctx->mouse_pos.x
        };
    }

    // click-on-track glide
    if (ctx->animation.widget == value)
        thumb_x = lerp_float(ctx->animation.value_key0, ctx->animation.value_key1, ctx->animation.t);

    // drag update
    if (ctx->mouse_down && ctx->dragging_object == value)
        thumb_x = ctx->mouse_pos.x - ctx->dragging_offset.x;

    thumb_x = clamp_float(track_rect.x, track_rect.x + track_rect.width, thumb_x);

    // keep the thumb geometry in sync with the (possibly) updated thumb position
    thumb_rect.x = thumb_x - grown_half;
    thumb_rect.y = center_y - grown_half;
    thumb_rect.width = grown_size;
    thumb_rect.height = grown_size;

    norm_value = (thumb_x - track_rect.x) / track_rect.width;
    *value = norm_value * (max_value - min_value) + min_value;
    *value = (step>0.f) ? roundf(*value / step) * step : *value;

    ctx->renderer.draw_box(thumb_rect.x, thumb_rect.y, thumb_rect.width, thumb_rect.height, grown_half,
                           ctx->colors.accent, ctx->renderer.user);

    ui_newline(ctx);
}

//-----------------------------------------------------------------------------------------------------------------------------
void ui_progress(ui_context* ctx, const char* label, float fraction, const char* fmt, ...)
{
    assert(ctx->current_window != NULL);

    fraction = clamp_float(0.f, 1.f, fraction);

    va_list args;
    va_start(args, fmt);
    vsnprintf(ctx->string_buffer, STRING_BUFFER_SIZE, fmt, args);
    va_end(args);

    // find the animation state for this bar, or create it already in place
    float animated = fraction;
    if (ctx->num_progress_states < MAX_PROGRESS)
    {
        uint32_t id = ui_hash(label, strlen(label));
        ui_progress_state* state = NULL;
        for (uint32_t i = 0; i < ctx->num_progress_states; ++i)
            if (ctx->progress_states[i].id == id)
                state = &ctx->progress_states[i];

        if (state == NULL)
        {
            state = &ctx->progress_states[ctx->num_progress_states++];
            state->id = id;
            state->animated = fraction;
        }

        state->animated = lerp_float(state->animated, fraction, 1.f - expf(-15.f * ctx->delta_time));
        if (fabsf(state->animated - fraction) < 0.001f)
            state->animated = fraction;
        animated = state->animated;
    }

    // label row : label on the left, formatted value on the right
    draw_align_text(ctx, &ctx->layout, label, ctx->colors.text, align_left);
    draw_align_text(ctx, &ctx->layout, ctx->string_buffer, ctx->colors.text, align_right);
    ui_newline(ctx);

    // bar row
    float center_y = ctx->layout.y + .5f * ctx->layout.height;

    ui_rect track_rect =
    {
        .x = ctx->layout.x,
        .y = center_y - ctx->padding * .5f,
        .width = ctx->layout.width,
        .height = ctx->padding
    };

    ctx->renderer.draw_box(track_rect.x, track_rect.y, track_rect.width, track_rect.height,
                           track_rect.height * .5f, ctx->colors.value_bg, ctx->renderer.user);

    float fill_width = animated * track_rect.width;
    if (fill_width > 0.f)
    {
        float fill_radius = fminf(track_rect.height * .5f, fill_width * .5f);
        ctx->renderer.draw_box(track_rect.x, track_rect.y, fill_width, track_rect.height,
                               fill_radius, ctx->colors.accent, ctx->renderer.user);
    }

    ui_newline(ctx);
}

//-----------------------------------------------------------------------------------------------------------------------------
bool ui_button(ui_context* ctx, const char* label, enum ui_text_alignment alignment)
{
    bool clicked = false;
    float label_width = text_width(ctx, label);

    ui_rect button_rect = {.y = ctx->layout.y, .height = ctx->font_height + ctx->padding, .width = label_width + 2.f * ctx->padding};

    switch(alignment)
    {
    case align_left: button_rect.x = ctx->layout.x; break;
    case align_center: button_rect.x = ctx->layout.x + ctx->layout.width * .5f - button_rect.width * .5f; break;
    default: button_rect.x = ctx->layout.x + ctx->layout.width - button_rect.width; break;
    }
    
    ui_vec2 text_pos = {button_rect.x + ctx->padding, button_rect.y + button_rect.height * .5f - ctx->font_height + ctx->padding};
    uint32_t button_color = ctx->colors.widget_bg;
    if (ctx->animation.widget == label)
    {
        button_color = lerp_color(ctx->colors.accent, ctx->colors.window_bg, ease_in_expo(ctx->animation.t));
        expand_rect(&button_rect, -ease_impulse(ctx->animation.t) * 2.f);
    }
    else if (ui_hovered(ctx, &button_rect))
    {
        if (ctx->mouse_button == button_pressed)
        {
            clicked = true;
            if (ctx->animation.widget != label)
                ctx->animation = (ui_animation) {.widget = (void*) label};
        }
        else
        {
            if (ctx->animation.widget == label && ctx->mouse_down)
                button_color = ctx->colors.accent;
            else if (ctx->hover.widget == label)
                button_color = lerp_color(ctx->colors.widget_bg, ctx->colors.widget_hover, ctx->hover.t);
            else
                ctx->hover = (ui_hover) {.widget = (void*) label};
        }
    }
    else if (ctx->hover.widget == label)
        ctx->hover.widget = NULL;

    // border
    ctx->renderer.draw_box(button_rect.x, button_rect.y, button_rect.width, button_rect.height, 
                           ctx->corner, ctx->colors.separator, ctx->renderer.user);

    expand_rect(&button_rect, -1.f);

    ctx->renderer.draw_box(button_rect.x, button_rect.y, button_rect.width, button_rect.height, 
                           ctx->corner, button_color, ctx->renderer.user);

    ctx->renderer.draw_text(text_pos.x, text_pos.y, label, strlen(label), ctx->colors.text, ctx->renderer.user);

    return clicked;
}

//-----------------------------------------------------------------------------------------------------------------------------
void ui_knob(ui_context* ctx, const char* label, float min_value, float max_value, float default_value, float* value)
{
    // knob needs more space
    ctx->layout.height = ctx->row_height * 2.f;

    float label_width = text_width(ctx, label);
    float width = ctx->layout.height * 2.f;
    float cx = ctx->layout.x + width * .5f;
    float cy = ctx->layout.y + ctx->layout.height * .25f + ctx->padding;
    float radius = ctx->layout.height * .25f;

    ui_rect knob_rect = {.x = cx - radius, .y = cy - radius, .width = radius * 2.f, .height = radius * 2.f};
    bool hovered = ui_hovered(ctx, &knob_rect);

    if (hovered && ctx->mouse_button == button_pressed)
    {
        ctx->dragging_object = value;
        ctx->dragging_offset.y = ctx->mouse_pos.y;
    }

    bool active = ctx->dragging_object == value;

    // default value and double click managment
    if (hovered && ctx->mouse_doubleclick)
    {
        ctx->animation = (ui_animation)
        {
            .value_key0 = *value,
            .value_key1 = default_value,
            .widget = value
        };
    }

    if (ctx->animation.widget == value)
    {
        *value = lerp_float(ctx->animation.value_key0, ctx->animation.value_key1, ctx->animation.t);
    }
    else if (active)
    {
        // if dragging, update the value
        float dx = ctx->dragging_offset.y - ctx->mouse_pos.y;
        ctx->dragging_offset.y = ctx->mouse_pos.y;
        float sens = ctx->font_height * 12.f;
        float v = *value + dx * (max_value - min_value) / sens;
        v = clamp_float(min_value, max_value, v);
        *value = v;
    }

    // draw bound dots
    float min_angle = -4.1887902f;
    float max_angle = 1.0471975512f;
    float bound_distance = radius * 1.05f;
    float bound_radius = radius * .1f;

    float bx1 = cx + cosf(min_angle) * bound_distance;
    float by1 = cy + sinf(min_angle) * bound_distance;
    float bx2 = cx + cosf(max_angle) * bound_distance;
    float by2 = cy + sinf(max_angle) * bound_distance;

    draw_disc(ctx, bx1, by1, bound_radius, ctx->colors.separator);
    draw_disc(ctx, bx2, by2, bound_radius, ctx->colors.separator);

    // knob rendering
    float inner_radius = radius * .8f;
    uint32_t knob_color = ctx->colors.widget_bg;
    if (active)
        knob_color = ctx->colors.widget_active;
    else if (hovered)
        knob_color = ctx->colors.widget_hover;

    draw_disc(ctx, cx, cy, radius, ctx->colors.window_border);
    draw_disc(ctx, cx, cy, inner_radius, knob_color);

    // draw value mark
    float t = (*value - min_value) / (max_value - min_value);
    t = clamp_float(0.f, 1.f, t);

    float angle = min_angle + t * (max_angle - min_angle);
    float mark_radius = inner_radius * 0.9f;
    float mx = cosf(angle);
    float my = sinf(angle);
    float line_width = ctx->padding / 8.f;

    ctx->renderer.draw_line(cx, cy, cx + mx * mark_radius, cy + my * mark_radius, line_width, ctx->colors.accent, ctx->renderer.user);
    ctx->renderer.draw_text(cx - label_width * .5f, cy + ctx->font_height, label, strlen(label), ctx->colors.text, ctx->renderer.user);

    ctx->layout.x += width;
    ctx->layout.width -= width;
}

//-----------------------------------------------------------------------------------------------------------------------------
const ui_rect* ui_get_layout(const ui_context* ctx)
{
    return &ctx->layout;
}

//-----------------------------------------------------------------------------------------------------------------------------
ui_rect ui_get_remaining(const ui_context* ctx)
{
    assert(ctx->current_window != NULL);
    const ui_window* w = ctx->current_window;

    return (ui_rect)
    {
        .x = w->pos.x + ctx->padding * 2.f,
        .y = ctx->layout.y,
        .width = w->width - (w->scrollbar_visible ? ctx->padding * 5.5f : ctx->padding * 4.f),
        .height = w->pos.y + w->height - ctx->padding - ctx->layout.y
    };
}

//-----------------------------------------------------------------------------------------------------------------------------
void ui_advance(ui_context* ctx, float dy)
{
    assert(ctx->current_window != NULL);
    ctx->layout.y += dy;
}

//-----------------------------------------------------------------------------------------------------------------------------
float ui_get_row_height(const ui_context* ctx)
{
    return ctx->row_height;
}

//-----------------------------------------------------------------------------------------------------------------------------
void ui_end_window(ui_context* ctx)
{
    assert(ctx->current_window != NULL);
    ctx->current_window->content_height = ctx->layout.y + ctx->current_window->scroll - ctx->content_rect.y;
    ctx->current_window = NULL;
    ctx->renderer.set_clip_rect(0, 0, UINT16_MAX, UINT16_MAX, ctx->renderer.user);
}

//-----------------------------------------------------------------------------------------------------------------------------
void ui_end_frame(ui_context* ctx)
{
    assert(ctx->current_window == NULL);

    if (ctx->animation.t >= 1.f)
    {
        ctx->animation.widget = NULL;
        ctx->animation.t = 1.f;
    }

    if (ctx->mouse_button == button_released)
    {
        ctx->resizing_window = NULL;
        ctx->dragging_object = NULL;
    }

    ctx->mouse_button = button_idle;
    ctx->mouse_wheel = 0.f;
}

