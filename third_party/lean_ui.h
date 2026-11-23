#ifndef __LEAN_UI_H__
#define __LEAN_UI_H__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

//-----------------------------------------------------------------------------------------------------------------------------
// public structures
//-----------------------------------------------------------------------------------------------------------------------------

enum ui_button_state
{
    button_idle,
    button_pressed,
    button_released
};

enum ui_text_alignment
{
    align_left,
    align_center,
    align_right
};

enum ui_window_option
{
    window_pinned = 1<<0,
    window_resizable = 1<<1
};

typedef struct 
{
    void (*draw_box)(float x, float y, float width, float height, float radius, uint32_t srgb_color, void* user);
    void (*draw_text)(float x, float y, const char* text, uint32_t srgb_color, void* user); // draw a text top-left aligned with x,y
    void (*draw_line)(float x0, float y0, float x1, float y1, float width, uint32_t srgb_color, void* user);
    void (*set_clip_rect)(uint16_t min_x, uint16_t min_y, uint16_t max_x, uint16_t max_y, void* user);
    float (*text_width)(const char* text, void* user);
    void* user;
} ui_renderer_fnc_t;

typedef struct
{
    void* preallocated_buffer;  // must be aligned on 8 bytes
    ui_renderer_fnc_t renderer_callbacks;
    float font_height;
} ui_def;

typedef struct {float x, y, width, height;} ui_rect;

typedef struct ui_context ui_context;

//-----------------------------------------------------------------------------------------------------------------------------
// api
//-----------------------------------------------------------------------------------------------------------------------------

#ifdef __cplusplus
extern "C" {
#endif

//-----------------------------------------------------------------------------------------------------------------------------
// Returns the number of bytes needed to allocate a ui_context and its buffer
size_t ui_min_memory_size(void);

//-----------------------------------------------------------------------------------------------------------------------------
// Initializes the library
//      [preallocated_buffer]   user-allocated memory of ui_min_memory_size() bytes, must be aligned on sizeof(uintptr_t)
//      [renderer_callbacks]    user-provided drawing callbacks
//      [font_height]           height in pixels of the font
ui_context* ui_init(const ui_def* def);

//-----------------------------------------------------------------------------------------------------------------------------
// Updates the position of mouse in screen coordinate in pixels
void ui_update_mouse_pos(ui_context* ctx, float x, float y);

//-----------------------------------------------------------------------------------------------------------------------------
// Updates the state of the primary (left) mouse button. Only this button is supported.
void ui_update_mouse_button(ui_context* ctx, enum ui_button_state button);

//-----------------------------------------------------------------------------------------------------------------------------
// [delta_time]    elapsed time in seconds since the previous frame
void ui_begin_frame(ui_context* ctx, float delta_time);

//-----------------------------------------------------------------------------------------------------------------------------
// Begins a new window
//      [name]                  unique name, hashed under the hood
//      [x, y, width, height]   initial position and size in pixels can be changed by the user
//      [options]               combination of options from enum ui_window_option
void ui_begin_window(ui_context* ctx, const char* name, float x, float y, float width, float height, uint32_t options);

//-----------------------------------------------------------------------------------------------------------------------------
// Displays text according to the alignment
//      [string]    can contains format expression (i.e %f) and additionnal parameters
void ui_text(ui_context* ctx, enum ui_text_alignment alignment, const char* string, ...);

//-----------------------------------------------------------------------------------------------------------------------------
// Moves the cursor to the next line, acts like a carriage return (CR)
void ui_newline(ui_context* ctx);

//-----------------------------------------------------------------------------------------------------------------------------
// Draws a subtle horizontal separator line
void ui_separator(ui_context* ctx);

//-----------------------------------------------------------------------------------------------------------------------------
// Displays a label on the left and a formatted value on the right
//
//      [label]     left-aligned text
//      [fmt]       printf-style format string for the value
void ui_value(ui_context* ctx, const char* label, const char* fmt, ...);

//-----------------------------------------------------------------------------------------------------------------------------
// Displays a toggle with a label
//      [value]     pointer to a bool, toggled on click
void ui_toggle(ui_context* ctx, const char* label, bool* value);

//-----------------------------------------------------------------------------------------------------------------------------
// Displays a segmented control with mutually exclusive options
//      [entries]       array of strings, must have a size of [num_entries]
//      [selected]      pointer to the index of the active segment
void ui_segmented(ui_context* ctx, const char** entries, uint32_t num_entries, uint32_t* selected);

//-----------------------------------------------------------------------------------------------------------------------------
// Displays a horizontal slider with a label
//      [min_value]    minimum allowed value
//      [max_value]    maximum allowed value
//      [step]         increment step (e.g. 0.1)
//      [value]        pointer to the controlled float
//      [fmt]          printf-style format for the displayed numeric value
void ui_slider(ui_context* ctx, const char* label, float min_value, float max_value, float step, float* value, const char* fmt);

//-----------------------------------------------------------------------------------------------------------------------------
// Displays a clickable button
//      [alignment]    horizontal alignment of the the button in the window
//
// returns true if the button was pressed this frame
bool ui_button(ui_context* ctx, const char* label, enum ui_text_alignment alignment);

//-----------------------------------------------------------------------------------------------------------------------------
// Displays a knob with a label below. Knobs are laid out from the current cursor position,
// so you can stack several in a row; each one needs enough remaining row width
// (the row height is doubled for the knob, so keep them in a row of their own and
// call ui_newline() after the last one of the row)
//      [min_value]     minimum allowed value
//      [max_value]     maximum allowed value (must be greater than min_value)
//      [default_value] double-click on the knob will reset the value to default
//      [value]         pointer to the controlled float
void ui_knob(ui_context* ctx, const char* label, float min_value, float max_value, float default_value, float* value);

//-----------------------------------------------------------------------------------------------------------------------------
// Returns the current layout rect, useful for custom rendering
const ui_rect* ui_get_layout(const ui_context* ctx);

//-----------------------------------------------------------------------------------------------------------------------------
// Ends the current window. Must match ui_begin_window()
void ui_end_window(ui_context* ctx);

//-----------------------------------------------------------------------------------------------------------------------------
// Ends the current frame. Must match ui_begin_frame(). Until next frame no more calls to lean_ui.
void ui_end_frame(ui_context* ctx);

#ifdef __cplusplus
}
#endif

#endif