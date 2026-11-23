#include "lean_ui.h"
#include <assert.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <math.h>

#define MAX_WINDOWS (16)
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


struct ui_context
{
    ui_vec2 mouse_pos;
    enum ui_button_state mouse_button;
    bool mouse_down;
    bool mouse_doubleclick;
    float doubleclick_timer;
    ui_window windows[MAX_WINDOWS];
    uint32_t num_windows;
    ui_window* current_window;
    ui_window* resizing_window;
    void* dragging_object;
    ui_animation animation;
    ui_hover hover;
    ui_vec2 dragging_offset;
    ui_rect layout;
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
static inline bool in_rect(const ui_rect *rect, ui_vec2 pos) 
{
    return ((pos.x>=rect->x) && (pos.x<=rect->x+rect->width) && (pos.y>=rect->y) && (pos.y<=rect->y+rect->height));
}
static inline void expand_rect(ui_rect* rect, float amount) {rect->x-=amount; rect->y-=amount; rect->width+=amount*2.f; rect->height+=amount*2.f;}
static inline float lerp_float(float a, float b, float t) {return fmaf(b - a, t, a);}
static inline float clamp_float(float min_value, float max_value, float f) {return fminf(max_value, fmaxf(min_value, f));}
static inline float ease_in_quad(float x) {return x * x;}
static inline float ease_in_cubic(float x) {return x * x * x;}
static inline float ease_impulse(float x) {return ease_in_cubic(sinf(x * 3.14159265f));}
static inline float ease_in_expo(float x){return (x == 0.f) ? 0.f : powf(2.f, 10.f * x - 10.f);}

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
static inline void draw_align_text(ui_context* ctx, const ui_rect* rect, const char* text, uint32_t srgb_color, enum ui_text_alignment alignment)
{
    if (alignment == align_left)
        ctx->renderer.draw_text(rect->x, rect->y, text, srgb_color, ctx->renderer.user);
    else
    {
        float text_width = ctx->renderer.text_width(text, ctx->renderer.user);
        if (alignment == align_right)
            ctx->renderer.draw_text(rect->x + rect->width - text_width, rect->y, text, srgb_color, ctx->renderer.user);
        else
            ctx->renderer.draw_text(rect->x + rect->width*.5f - text_width*.5f, rect->y, text, srgb_color, ctx->renderer.user);
    }
}

//-----------------------------------------------------------------------------------------------------------------------------
static inline void draw_disc(ui_context* ctx, float x, float y, float radius, uint32_t srgb_color)
{
    ctx->renderer.draw_box(x - radius, y - radius, radius*2.f, radius*2.f, radius, srgb_color, ctx->renderer.user);
}

//-----------------------------------------------------------------------------------------------------------------------------
// UI functions
//-----------------------------------------------------------------------------------------------------------------------------

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
        .corner = fmaxf(def->font_height/2.f, 2.f),
        .colors = 
        {
            .window_bg = 0xFFF7F0E9,
            .window_border = 0xFFE3CDB8,
            .title_bg = 0xFFE5C4A8,
            .title_text = 0xFF402A1B,
            .widget_bg = 0xFFF4E6D8,
            .widget_hover = 0xFFEEDBC3,
            .widget_active = 0xFFE7CCB0,
            .text = 0xFF553E2C,
            .accent = 0xFFE68B3E,
            .value_bg = 0xFFEDE6DE,
            .value_text = 0xFF1E1E1E,
            .separator = 0x40553E2C
        }
    };
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
void ui_begin_frame(ui_context* ctx, float delta_time)
{
    ctx->current_window = NULL;

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
            .min_width = ctx->renderer.text_width(name, ctx->renderer.user) + ctx->padding * 2.f,
            .min_height = ctx->row_height * 2.f,
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

    ui_rect title_rect = {w->pos.x+ctx->padding, w->pos.y+ctx->padding, w->width-ctx->padding*2.f, ctx->row_height};

    // move the window if click on the title bar
    if (ctx->mouse_button == button_pressed && in_rect(&title_rect, ctx->mouse_pos) &&
        !(w->options&window_pinned) && ctx->resizing_window != w && ctx->dragging_object == NULL)
    {
        ctx->dragging_object = w;
        ctx->dragging_offset = ui_vec2_sub(ctx->mouse_pos, w->pos);
    }

    if (ctx->mouse_down && ctx->dragging_object == w)
        w->pos = ui_vec2_sub(ctx->mouse_pos, ctx->dragging_offset);

    // border
    ctx->renderer.draw_box(w->pos.x, w->pos.y, w->width, w->height, ctx->corner, ctx->colors.window_border, ctx->renderer.user);

    // title bg
    ctx->renderer.draw_box(title_rect.x, title_rect.y, title_rect.width, title_rect.height, ctx->corner, ctx->colors.title_bg, ctx->renderer.user);

    ctx->layout = (ui_rect)
    {
        .x = w->pos.x + ctx->padding,
        .y = title_rect.y + title_rect.height + ctx->padding,
        .width = w->width - ctx->padding*2.f,
        .height = ctx->row_height
    };

    // background
    ctx->renderer.draw_box(ctx->layout.x, ctx->layout.y, ctx->layout.width, 
                           w->height - title_rect.height - ctx->padding*3.f, 0, ctx->colors.window_bg, ctx->renderer.user);
    
    ctx->layout.x += ctx->padding;
    ctx->layout.width -= 2.f * ctx->padding;

    // title
    draw_align_text(ctx, &title_rect, w->name, ctx->colors.title_text, align_center);

    // draw resize handle
    if (w->options&window_resizable)
        ctx->renderer.draw_box(handle_rect.x, handle_rect.y, handle_rect.width, handle_rect.height, 0.f, ctx->colors.separator, ctx->renderer.user);

    // clip rect
    uint16_t clip_minx = (uint16_t) fmaxf(ctx->layout.x, 0.f);
    uint16_t clip_miny = (uint16_t) fmaxf(ctx->layout.y, 0.f);
    uint16_t clip_maxx = (uint16_t) (ctx->layout.x + ctx->layout.width + .5f);
    uint16_t clip_maxy = (uint16_t) (w->pos.y + w->height - ctx->padding + .5f);
    ctx->renderer.set_clip_rect(clip_minx, clip_miny, clip_maxx, clip_maxy, ctx->renderer.user);
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
void ui_newline(ui_context* ctx)
{
    assert(ctx->current_window != NULL);
    ctx->layout.x = ctx->current_window->pos.x + ctx->padding * 2.f;
    ctx->layout.y += ctx->layout.height;
    ctx->layout.width = ctx->current_window->width - ctx->padding * 4.f;
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

    ctx->renderer.draw_text(ctx->layout.x, ctx->layout.y, label, ctx->colors.text, ctx->renderer.user);
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

    if (in_rect(&track_rect, ctx->mouse_pos) && ctx->mouse_button == button_pressed)
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

    if (in_rect(&track_rect, ctx->mouse_pos))
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
        if (in_rect(&button_rect, ctx->mouse_pos))
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

    // thumb geometry
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

    const bool track_hovered = in_rect(&track_rect, ctx->mouse_pos);
    const bool thumb_hovered = in_rect(&thumb_rect, ctx->mouse_pos);

    // track change color on mouse-over
    uint32_t track_color = (track_hovered || (ctx->dragging_object == value)) ? ctx->colors.widget_hover : ctx->colors.widget_bg;
    ctx->renderer.draw_box(track_rect.x, track_rect.y, track_rect.width, track_rect.height,
                           track_rect.height*.5f, track_color, ctx->renderer.user);

    // thumb mouse-over and dragging
    if (thumb_hovered)
    {
        expand_rect(&thumb_rect, 2.f);
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
    thumb_rect.x = thumb_x - half_size;

    norm_value = (thumb_x - track_rect.x) / track_rect.width;
    *value = norm_value * (max_value - min_value) + min_value;
    *value = (step>0.f) ? roundf(*value / step) * step : *value;

    ctx->renderer.draw_box(thumb_rect.x, thumb_rect.y, thumb_rect.width, thumb_rect.height, half_size,
                           ctx->colors.accent, ctx->renderer.user);

    ui_newline(ctx);
}

//-----------------------------------------------------------------------------------------------------------------------------
bool ui_button(ui_context* ctx, const char* label, enum ui_text_alignment alignment)
{
    bool clicked = false;
    float text_width = ctx->renderer.text_width(label, ctx->renderer.user);

    ui_rect button_rect = {.y = ctx->layout.y, .height = ctx->font_height + ctx->padding, .width = text_width + 2.f * ctx->padding};

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
    else if (in_rect(&button_rect, ctx->mouse_pos))
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

    ctx->renderer.draw_text(text_pos.x, text_pos.y, label, ctx->colors.text, ctx->renderer.user);

    return clicked;
}

//-----------------------------------------------------------------------------------------------------------------------------
void ui_knob(ui_context* ctx, const char* label, float min_value, float max_value, float default_value, float* value)
{
    // knob needs more space
    ctx->layout.height = ctx->row_height * 2.f;

    float text_width = ctx->renderer.text_width(label, ctx->renderer.user);
    float width = ctx->layout.height * 2.f;
    float cx = ctx->layout.x + width * .5f;
    float cy = ctx->layout.y + ctx->layout.height * .25f + ctx->padding;
    float radius = ctx->layout.height * .25f;

    ui_rect knob_rect = {.x = cx - radius, .y = cy - radius, .width = radius * 2.f, .height = radius * 2.f};
    bool hovered = in_rect(&knob_rect, ctx->mouse_pos);

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
    ctx->renderer.draw_text(cx - text_width * .5f, cy + ctx->font_height, label, ctx->colors.text, ctx->renderer.user);

    ctx->layout.x += width;
    ctx->layout.width -= width;
}

//-----------------------------------------------------------------------------------------------------------------------------
const ui_rect* ui_get_layout(const ui_context* ctx)
{
    return &ctx->layout;
}

//-----------------------------------------------------------------------------------------------------------------------------
void ui_end_window(ui_context* ctx)
{
    assert(ctx->current_window != NULL);
    ctx->current_window->min_height = ctx->layout.y - ctx->current_window->pos.y + ctx->row_height * 2.f;
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
}

