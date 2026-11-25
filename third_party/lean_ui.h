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
    window_pinned = 1<<0,          // the window cannot be moved by dragging its title bar
    window_resizable = 1<<1,       // the window can be resized from its bottom-right corner
    window_no_titlebar = 1<<2,     // no title bar : the content starts at the top and the window has no drag handle
    window_no_border = 1<<3,       // no border ring : the window is a single rounded card
    window_no_chrome = window_no_titlebar | window_no_border
};

typedef struct
{
    void (*draw_box)(float x, float y, float width, float height, float radius, uint32_t srgb_color, void* user);
    // [text] is a view of [length] bytes, not guaranteed to be null-terminated
    void (*draw_text)(float x, float y, const char* text, size_t length, uint32_t srgb_color, void* user); // draw the view top-left aligned with x,y
    void (*draw_line)(float x0, float y0, float x1, float y1, float width, uint32_t srgb_color, void* user);
    void (*set_clip_rect)(uint16_t min_x, uint16_t min_y, uint16_t max_x, uint16_t max_y, void* user);
    float (*text_width)(const char* text, size_t length, void* user);   // width of the [length] bytes view of [text]
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
// Updates the mouse wheel. [delta] is in wheel notches, positive means scroll up (like GLFW's yoffset).
// Calls add up until ui_end_frame, where the window under the cursor consumes them.
void ui_update_mouse_wheel(ui_context* ctx, float delta);

//-----------------------------------------------------------------------------------------------------------------------------
// [delta_time]    elapsed time in seconds since the previous frame
void ui_begin_frame(ui_context* ctx, float delta_time);

//-----------------------------------------------------------------------------------------------------------------------------
// Begins a new window
//      [name]                  unique name, hashed under the hood
//      [x, y, width, height]   initial position and size in pixels can be changed by the user
//      [options]               combination of options from enum ui_window_option
//
// A window created with window_no_titlebar has no drag handle and cannot be moved.
void ui_begin_window(ui_context* ctx, const char* name, float x, float y, float width, float height, uint32_t options);

//-----------------------------------------------------------------------------------------------------------------------------
// Displays text according to the alignment
//      [string]    can contains format expression (i.e %f) and additionnal parameters
void ui_text(ui_context* ctx, enum ui_text_alignment alignment, const char* string, ...);

//-----------------------------------------------------------------------------------------------------------------------------
// Displays wrapped text, always left-aligned, no formatting
//      [text]      UTF-8 string view of [length] bytes, not required to be null-terminated, never modified
//
// The [length] bytes are wrapped on spaces to the window's content width and re-wrapped every frame.
// '\n' forces a line break. A word wider than the window overflows and is clipped by the window's clip rect.
void ui_paragraph(ui_context* ctx, const char* text, size_t length);

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
// Returns the rect of the remaining content area of the current window : from the top of the
// current row, at the full content width, down to the bottom edge of the window's content.
// Call it at a row boundary (right after ui_begin_window() or ui_newline()). Useful for custom
// rendering, e.g. embedding a panel from another layout engine
ui_rect ui_get_remaining(const ui_context* ctx);

//-----------------------------------------------------------------------------------------------------------------------------
// Moves the layout cursor down by [dy] pixels. Call it after rendering custom content of that
// height at the cursor position, so that the next widget is placed below it
void ui_advance(ui_context* ctx, float dy);

//-----------------------------------------------------------------------------------------------------------------------------
// Returns the height in pixels of a row
float ui_get_row_height(const ui_context* ctx);

//-----------------------------------------------------------------------------------------------------------------------------
// Ends the current window. Must match ui_begin_window()
//
// Content taller than the window's visible area scrolls : feed ui_update_mouse_wheel() to scroll,
// or drag the scrollbar in the right gutter (it appears only when the content overflows). To keep
// a window from scrolling, size it to fit its content. The last row counts towards the content
// height only if it ends with ui_newline(), like knobs already require.
void ui_end_window(ui_context* ctx);

//-----------------------------------------------------------------------------------------------------------------------------
// Ends the current frame. Must match ui_begin_frame(). Until next frame no more calls to lean_ui.
void ui_end_frame(ui_context* ctx);

#ifdef __cplusplus
}
#endif

#endif