// examples/clay.c
//
// A VS Code-like editor UI : clay computes the layout, onedraw draws it.
// The code view is read-only : this app exercises the clay layout interface,
// not real editing.

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define SOKOL_METAL
#include "sokol_app.h"

#define CLAY_IMPLEMENTATION
#include "clay.h"

#include "../lib/onedraw.h"

#include "vmem.h"

#define COUNOF(a)   (sizeof(a) / sizeof((a)[0]))

//----------------------------------------------------------------------------------------------------------------------------
// theme (light), colors are 0xRRGGBB

#define THEME_SIDEBAR      (0xf3f3f3u)
#define THEME_HOVER        (0xe7e7e7u)
#define THEME_SELECTED     (0xd6ebffu)
#define THEME_ACCENT       (0x007accu)
#define THEME_EDITOR       (0xffffffu)
#define THEME_TABBAR       (0xf3f3f3u)
#define THEME_TAB_HOVER    (0xeaeaeau)
#define THEME_BORDER       (0xd9d9d9u)
#define THEME_STATUS       (0x007accu)
#define THEME_BUTTON       (0xe1e1e1u)
#define THEME_BUTTON_HOVER (0xd0d0d0u)

#define THEME_TEXT         (0x1e1e1eu)
#define THEME_TEXT_MUTED   (0x6e6e6eu)
#define THEME_LINENUM      (0x9e9e9eu)
#define THEME_COMMENT      (0x008000u)
#define THEME_TERM_TEXT    (0x333333u)
#define THEME_TERM_PROMPT  (0x2e7d32u)
#define THEME_TERM_DIM     (0x737373u)
#define THEME_SCROLLBAR    (0xc9c9c9u)
#define THEME_SCROLLBAR_DRAG (0x9c9c9cu)
#define THEME_GUIDE        (0xd7d7d7u)
#define THEME_GLYPH_FOLDER (0xd0a558u)
#define THEME_GLYPH_FILE_C (0x4f8cc9u)
#define THEME_GLYPH_FILE_MD (0x58a05au)
#define THEME_GLYPH_FILE_CMAKE (0xc9714fu)
#define THEME_GLYPH_FILE   (0x8a8a8au)

// clay passes fontId through to the render commands, it is mapped to an onedraw family in font_from_id()
#define FONT_UI      (0)   // od_font_regular
#define FONT_MONO    (1)   // od_font_mono
#define FONT_BOLD    (2)   // od_font_bold
#define FONT_ITALIC  (3)   // od_font_italic

#define UI_FONT_SIZE    (14)
#define SMALL_FONT_SIZE (11)
#define CODE_FONT_SIZE  (14)
#define TERM_FONT_SIZE  (13)
#define LINE_PITCH      (22)

#define TAB_BAR_HEIGHT   (34)
#define STATUS_BAR_HEIGHT (22)
#define TERMINAL_LINE_INTERVAL (0.45f)
#define SPLITTER_WIDTH   (4)

#define CLAMP(x, lo, hi)  (((x) < (lo)) ? (lo) : (((x) > (hi)) ? (hi) : (x)))

//----------------------------------------------------------------------------------------------------------------------------
// color conversions

// onedraw colors are packed 0xAABBGGRR
static draw_color od_html(uint32_t html)
{
    return (draw_color)(0xff000000u | ((html & 0xffu) << 16) | (((html >> 8) & 0xffu) << 8) | ((html >> 16) & 0xffu));
}

static Clay_Color clay_html(uint32_t html)
{
    return (Clay_Color){ (float)((html >> 16) & 0xffu), (float)((html >> 8) & 0xffu), (float)(html & 0xffu), 255.f };
}

// alpha 0 : clay skips the rectangle render command entirely
static Clay_Color clay_transparent(void)
{
    return (Clay_Color){ 0.f, 0.f, 0.f, 0.f };
}

static draw_color od_color(Clay_Color c)
{
    uint32_t r = (uint32_t)(c.r + 0.5f);
    uint32_t g = (uint32_t)(c.g + 0.5f);
    uint32_t b = (uint32_t)(c.b + 0.5f);
    return (draw_color)(0xff000000u | (b << 16) | (g << 8) | r);
}

//----------------------------------------------------------------------------------------------------------------------------
// the fake project files shown in the tree and the code view

struct fake_file
{
    const char* name;
    const char* const* lines;
    uint32_t num_lines;
};

static const char* main_c_lines[] = {
    "#include <stdlib.h>",
    "#include <stdio.h>",
    "",
    "#define SOKOL_METAL",
    "#include \"sokol_app.h\"",
    "",
    "#include \"onedraw.h\"",
    "",
    "struct onedraw* renderer;",
    "static void* od_buffer = NULL;",
    "",
    "void init(void)",
    "{",
    "    sapp_environment env = sapp_get_environment();",
    "",
    "    od_buffer = malloc(od_min_memory_size());",
    "    renderer = od_init(&(onedraw_def)",
    "    {",
    "        .metal_device = (void*)env.metal.device,",
    "        .preallocated_buffer = od_buffer,",
    "        .viewport_width = (uint32_t)sapp_width(),",
    "        .viewport_height = (uint32_t)sapp_height(),",
    "        .srgb_backbuffer = false,",
    "        .allow_screenshot = true",
    "    });",
    "",
    "    if (renderer == NULL)",
    "        exit(1);",
    "}",
    "",
    "void frame(void)",
    "{",
    "    if (renderer == NULL)",
    "        return;",
    "",
    "    od_begin_frame(renderer);",
    "    od_draw_box(renderer, 20, 20, 220, 140, 12, 0xff4a90d9);",
    "    od_draw_text(renderer, 32, 64, \"hello onedraw\", od_font_regular, 20, 0xff1e1e1e);",
    "    od_draw_disc(renderer, 320, 80, 48, 0xffe3c054);",
    "    od_draw_ring(renderer, 480, 80, 36, 4, 0xffc65046);",
    "    od_draw_line(renderer, 20, 200, 500, 200, 2, 0xff878573);",
    "    od_end_frame(renderer, (void*)sapp_acquire_swapchain().metal.current_drawable);",
    "}",
    "",
    "void cleanup(void)",
    "{",
    "    if (renderer != NULL)",
    "        od_terminate(renderer);",
    "    free(od_buffer);",
    "}",
    "",
    "sapp_desc sokol_main(int argc, char* argv[])",
    "{",
    "    (void)argc;",
    "    (void)argv;",
    "    return (sapp_desc)",
    "    {",
    "        .width = 960,",
    "        .height = 540,",
    "        .init_cb = init,",
    "        .frame_cb = frame,",
    "        .cleanup_cb = cleanup,",
    "        .high_dpi = true,",
    "        .window_title = \"onedraw\"",
    "    };",
    "}",
};

static const char* editor_c_lines[] = {
    "// the clay layout of this very app : a simplified copy of examples/clay.c",
    "",
    "#include \"clay.h\"",
    "",
    "struct onedraw* renderer;",
    "",
    "Clay_Dimensions measure_text(Clay_StringSlice text, Clay_TextElementConfig* config, void* user_data)",
    "{",
    "    od_font font = font_from_id(config->fontId);",
    "    float size = (float)config->fontSize;",
    "",
    "    return (Clay_Dimensions)",
    "    {",
    "        .width = od_string_view_width(renderer, text.chars, (size_t)text.length, font, size),",
    "        .height = od_text_line_height(renderer, font, size)",
    "    };",
    "}",
    "",
    "void declare_layout(void)",
    "{",
    "    Clay_BeginLayout();",
    "",
    "    CLAY(CLAY_ID(\"root\"),",
    "    {",
    "        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) }, .layoutDirection = CLAY_TOP_TO_BOTTOM }",
    "    })",
    "    {",
    "        CLAY(CLAY_ID(\"title_bar\"),",
    "        {",
    "            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(40) } },",
    "            .backgroundColor = { 228, 228, 228, 255 }",
    "        })",
    "        {",
    "            CLAY_TEXT(\"onedraw editor\", { .fontId = 0, .fontSize = 13 });",
    "        }",
    "",
    "        CLAY(CLAY_ID(\"sidebar\"),",
    "        {",
    "            .layout = { .sizing = { CLAY_SIZING_FIXED(220), CLAY_SIZING_GROW(0) } }",
    "        })",
    "        {",
    "            CLAY_TEXT(\"- src\", { .fontId = 0, .fontSize = 13 });",
    "            CLAY_TEXT(\"    main.c\", { .fontId = 0, .fontSize = 13 });",
    "            CLAY_TEXT(\"    editor.c\", { .fontId = 0, .fontSize = 13 });",
    "        }",
    "    }",
    "",
    "    Clay_RenderCommandArray commands = Clay_EndLayout(delta_time);",
    "",
    "    for (int32_t i = 0; i < commands.length; i++)",
    "    {",
    "        Clay_RenderCommand* cmd = Clay_RenderCommandArray_Get(&commands, i);",
    "        draw_command(cmd);   // rectangles, text and clips become onedraw calls",
    "    }",
    "}",
};

static const char* scene_c_lines[] = {
    "// a static scene used to exercise the onedraw primitives,",
    "// see tests/scene.c for the full version",
    "",
    "#include \"onedraw.h\"",
    "",
    "void scene_init(struct onedraw* r)",
    "{",
    "    od_set_clear_color(r, 0xffebecdc);",
    "    od_set_canvas(r, 0.f, 0.f, 2560.f, 1440.f);",
    "}",
    "",
    "void scene_draw(struct onedraw* r, uint32_t frame_count)",
    "{",
    "    float t = (float)frame_count * 0.02f;   // unused, the scene is static",
    "",
    "    od_draw_box(r, 40, 40, 700, 360, 24, 0xff2485a6);",
    "    od_draw_box(r, 480, 220, 1100, 640, 24, 0xffe3c054);",
    "",
    "    od_draw_disc(r, 160 + (float)(frame_count % 600), 200, 64, 0xffc65046);",
    "    od_draw_disc_gradient(r, 400, 480, 120, 0xff1e7453, 0xffa1bf41);",
    "",
    "    od_draw_ring(r, 900, 180, 80, 12, 0xff284261);",
    "    od_draw_ellipse(r, 900, 500, 1100, 560, 40, 0xff754d45);",
    "",
    "    float points[6] = { 1300, 120, 1500, 400, 1100, 400 };",
    "    od_draw_triangle(r, points, 8, 0xff54bad2);",
    "",
    "    od_draw_capsule(r, 40, 700, 700, 700, 10, 0xff5f5854);",
    "",
    "    od_draw_text(r, 40, 760, \"onedraw : single draw call 2D renderer\", od_font_regular, 32, 0xff232228);",
    "    od_draw_text(r, 40, 820, \"GPU-driven sdf shapes, anti-aliased\", od_font_italic, 28, 0xff5f5854);",
    "",
    "    od_push_cliprect(r, 40, 900, 1240, 1100);",
    "    od_draw_box(r, 40, 900, 1240, 1100, 0, 0xffc3d5c7);",
    "    od_draw_arc(r, 640, 1000, 1.f, 0.f, 3.14159f, 60, 8, 0xff2485a6);",
    "    od_pop_clip(r);",
    "}",
};

static const char* readme_lines[] = {
    "# onedraw",
    "",
    "**GPU-driven sdf 2D renderer drop-in library**",
    "",
    "onedraw renders everything in a single indirect draw call,",
    "plus one more for cached tiles when the tile cache is on.",
    "",
    "It relies on the Apple Metal API, wave instructions, and",
    "indirect draw calls.",
    "",
    "## Features",
    "",
    "* **Single draw call rendering** - all primitives are",
    "  rasterized by a single indirect draw call",
    "* **Hierarchical tile-based binning** - the compute shader",
    "  processes only the primitives that influence each tile",
    "* **Tile cache** - opt-in via the `cache` flag : unchanged",
    "  tiles are redrawn from a full-screen cache texture",
    "* **Offscreen rendering** - render into a library-owned",
    "  texture, no window required",
    "* **Anti-aliased** - smooth edges using signed distance",
    "  functions",
    "* **Text** - four built-in font families baked into one",
    "  BC4 atlas, UTF-8, metrics provided for layout",
    "",
    "## Integration",
    "",
    "1. Copy all files from the `/lib` folder.",
    "2. Add `onedraw.cpp` to your build system.",
    "3. Create a window and provide the Metal device and",
    "   drawable object.",
    "4. Link with the Metal framework.",
};

static const char* cmake_lines[] = {
    "cmake_minimum_required(VERSION 3.25)",
    "project(onedraw)",
    "",
    "if(NOT CMAKE_BUILD_TYPE)",
    "    set(CMAKE_BUILD_TYPE Debug CACHE STRING \"Choose the type of build.\" FORCE)",
    "endif()",
    "",
    "set(CMAKE_CXX_STANDARD 17)",
    "set(CMAKE_C_STANDARD 99)",
    "set(CMAKE_C_FLAGS_RELEASE \"-O3 -Wall -Wextra\")",
    "",
    "# --- pre-build : compiles the metal shaders and bakes the fonts ---",
    "add_subdirectory(pre-build)",
    "",
    "# --- minimal example ---",
    "add_executable(minimal",
    "    ./lib/onedraw.cpp",
    "    ./tests/minimal.c",
    "    ./tests/sokol_app.mm",
    ")",
    "",
    "target_include_directories(minimal PRIVATE ./third_party)",
    "",
    "if(TARGET build_lib)",
    "    add_dependencies(minimal build_lib)",
    "endif()",
    "",
    "target_link_libraries(minimal",
    "    \"-framework Metal\"",
    "    \"-framework MetalKit\"",
    "    \"-framework AppKit\"",
    "    \"-framework Foundation\"",
    "    \"-framework QuartzCore\"",
    ")",
    "",
    "# --- test app ---",
    "add_executable(test",
    "    ./lib/onedraw.cpp",
    "    ./tests/test.c",
    "    ./tests/sokol_app.mm",
    "    ./tests/scene.c",
    "    ./tests/compare.c",
    ")",
};

static const struct fake_file fake_files[] = {
    { "main.c",         main_c_lines, COUNOF(main_c_lines) },
    { "editor.c",       editor_c_lines, COUNOF(editor_c_lines) },
    { "scene.c",        scene_c_lines, COUNOF(scene_c_lines) },
    { "README.md",      readme_lines, COUNOF(readme_lines) },
    { "CMakeLists.txt", cmake_lines, COUNOF(cmake_lines) },
};

#define NUM_FAKE_FILES COUNOF(fake_files)

// the files shown in the tab bar
static const uint32_t tab_files[] = { 0, 1, 2 };
#define NUM_TABS COUNOF(tab_files)

//----------------------------------------------------------------------------------------------------------------------------
// the file tree

struct tree_entry
{
    const char* label;
    bool is_folder;
    bool expanded;
    uint8_t depth;
    uint32_t file_index;   // index into fake_files for file entries
};

static struct tree_entry tree[] = {
    { "src",            true,  true,  0, UINT32_MAX },
    { "main.c",         false, false, 1, 0 },
    { "editor.c",       false, false, 1, 1 },
    { "scene.c",        false, false, 1, 2 },
    { "assets",         true,  false, 0, UINT32_MAX },
    { "fonts",          true,  false, 1, UINT32_MAX },
    { "textures",       true,  false, 1, UINT32_MAX },
    { "README.md",      false, false, 0, 3 },
    { "CMakeLists.txt", false, false, 0, 4 },
};

#define NUM_TREE_ENTRIES COUNOF(tree)

//----------------------------------------------------------------------------------------------------------------------------
// the fake build output appended to the terminal over time

struct term_line
{
    const char* text;
    uint32_t color;
};

static const struct term_line terminal_script[] = {
    { "$ cmake -B build -S . -DCMAKE_BUILD_TYPE=Release", THEME_TERM_PROMPT },
    { "-- The C compiler identification is AppleClang 16.0.0.16000027", THEME_TERM_DIM },
    { "-- Detecting C compiler ABI info - done", THEME_TERM_DIM },
    { "-- Check for working C compiler - done", THEME_TERM_DIM },
    { "-- Configuring done (0.3s)", THEME_TERM_DIM },
    { "-- Generating done (0.1s)", THEME_TERM_DIM },
    { "-- Build files have been written to: build", THEME_TERM_DIM },
    { "", THEME_TERM_TEXT },
    { "$ cmake --build build", THEME_TERM_PROMPT },
    { "[ 14%] Building CXX object CMakeFiles/editor.dir/lib/onedraw.cpp.o", THEME_TERM_TEXT },
    { "[ 42%] Building C object CMakeFiles/editor.dir/examples/clay.c.o", THEME_TERM_TEXT },
    { "[ 71%] Linking CXX executable editor", THEME_TERM_TEXT },
    { "[100%] Built target editor", THEME_ACCENT },
    { "", THEME_TERM_TEXT },
    { "$ ./build/editor", THEME_TERM_PROMPT },
    { "[onedraw] metal device : Apple M3 Pro", THEME_TERM_DIM },
    { "[onedraw] viewport : 1280x720 @ 2x", THEME_TERM_DIM },
    { "[onedraw] gpu memory : 48.3 MiB", THEME_TERM_DIM },
    { "", THEME_TERM_TEXT },
    { "Ready.", THEME_TERM_PROMPT },
};

#define NUM_TERMINAL_LINES COUNOF(terminal_script)

//----------------------------------------------------------------------------------------------------------------------------
// app state

struct onedraw* renderer = NULL;
struct linear_allocator memory;   // onedraw buffer + clay arena

uint32_t selected_file = 0;
bool mouse_down = false;
float mouse_x = 0.f;
float mouse_y = 0.f;
Clay_Vector2 pending_scroll = { 0.f, 0.f };

// pane sizes, changed by dragging the splitters
float sidebar_width = 220.f;
float terminal_height = 200.f;
uint32_t dragging_splitter = 0;   // 0 = none, 1 = vertical (sidebar width), 2 = horizontal (terminal height)

// the scrollbars are drawn and hit-tested by the app, clay only tracks the scroll position
struct scrollbar
{
    Clay_BoundingBox thumb;   // last computed thumb box, in points
    bool dragging;
    float grab_offset;        // pointer y minus thumb y at grab time
};
static struct scrollbar scrollbars[2];   // [0] = code view, [1] = terminal

uint32_t terminal_lines = 0;
float terminal_timer = 0.f;
bool terminal_follow = true;   // pin the view to the tail until the user scrolls manually
bool code_scroll_to_top = false;

//----------------------------------------------------------------------------------------------------------------------------
static od_font font_from_id(uint16_t id)
{
    switch (id)
    {
    case FONT_MONO:   return od_font_mono;
    case FONT_BOLD:   return od_font_bold;
    case FONT_ITALIC: return od_font_italic;
    default:          return od_font_regular;
    }
}

// clay measures every text element through this function, onedraw provides the metrics
static Clay_Dimensions measure_text(Clay_StringSlice text, Clay_TextElementConfig* config, void* user_data)
{
    struct onedraw* r = (struct onedraw*)user_data;
    od_font font = font_from_id(config->fontId);
    float size = (float)config->fontSize;

    return (Clay_Dimensions)
    {
        .width = od_string_view_width(r, text.chars, (size_t)text.length, font, size),
        .height = od_text_line_height(r, font, size)
    };
}

static void select_file(uint32_t index)
{
    if (index == selected_file)
        return;
    selected_file = index;
    code_scroll_to_top = true;
}

// the fake build output appends one line every TERMINAL_LINE_INTERVAL seconds
static void update_terminal(float dt)
{
    if (terminal_lines >= NUM_TERMINAL_LINES)
        return;

    terminal_timer += dt;
    while ((terminal_timer >= TERMINAL_LINE_INTERVAL) && (terminal_lines < NUM_TERMINAL_LINES))
    {
        terminal_timer -= TERMINAL_LINE_INTERVAL;
        terminal_lines++;
    }
}

static double last_time = 0.0;

static float frame_delta_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
    float dt = (float)(now - last_time);
    last_time = now;
    return (dt > 0.f) ? dt : (1.f / 60.f);
}

static void clay_error(Clay_ErrorData data)
{
    fprintf(stderr, "clay error (%d) : %.*s\n", (int)data.errorType, data.errorText.length, data.errorText.chars);
}

static void custom_log(const char* string)
{
    fprintf(stdout, "%s\n", string);
}

//----------------------------------------------------------------------------------------------------------------------------
// layout declaration

static void declare_sidebar(bool clicked)
{
    CLAY(CLAY_ID("sidebar"),
    {
        .layout = { .sizing = { CLAY_SIZING_FIXED(sidebar_width), CLAY_SIZING_GROW(0) }, .layoutDirection = CLAY_TOP_TO_BOTTOM, .padding = { 12, 8, 8, 0 }, .childGap = 4 },
        .backgroundColor = clay_html(THEME_SIDEBAR),
        .border = { .color = clay_html(THEME_BORDER), .width = { 0, 1, 0, 0, 0 } }
    })
    {
        CLAY_TEXT(CLAY_STRING("EXPLORER"),
        {
            .fontId = FONT_BOLD,
            .fontSize = SMALL_FONT_SIZE,
            .textColor = clay_html(THEME_TEXT_MUTED),
            .wrapMode = CLAY_TEXT_WRAP_NONE
        });

        // a collapsed folder hides every entry below it until the depth drops back to its own
        int32_t hidden_below = INT32_MAX;   // INT32_MAX = nothing is hidden
        for (uint32_t i = 0; i < NUM_TREE_ENTRIES; i++)
        {
            struct tree_entry* e = &tree[i];
            if ((int32_t)e->depth >= hidden_below)
                continue;
            if (e->is_folder && !e->expanded)
                hidden_below = (int32_t)e->depth + 1;
            else
                hidden_below = INT32_MAX;

            // the node is a CUSTOM element : clay places it (and the app hit-tests it),
            // draw_tree_node() paints it with onedraw primitives
            CLAY(CLAY_IDI("tree_entry", i),
            {
                .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(24) } },
                .custom = { .customData = (void*)(uintptr_t)(i + 1) }   // +1 : a NULL customData would skip the command
            })
            {
                if (Clay_Hovered() && clicked)
                {
                    if (e->is_folder)
                        e->expanded = !e->expanded;
                    else
                        select_file(e->file_index);
                }
            }
        }
    }
}

static void declare_tab_bar(bool clicked)
{
    CLAY(CLAY_ID("tab_bar"),
    {
        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(TAB_BAR_HEIGHT) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .padding = { 4, 0, 0, 0 } },
        .backgroundColor = clay_html(THEME_TABBAR),
        .border = { .color = clay_html(THEME_BORDER), .width = { 0, 0, 0, 1, 0 } }
    })
    {
        for (uint32_t i = 0; i < NUM_TABS; i++)
        {
            bool is_active = (tab_files[i] == selected_file);

            CLAY(CLAY_IDI("tab", i),
            {
                .layout = { .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIXED(TAB_BAR_HEIGHT) }, .padding = { 12, 12, 0, 0 }, .childAlignment = { CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER } },
                .backgroundColor = is_active ? clay_html(THEME_EDITOR) : (Clay_Hovered() ? clay_html(THEME_TAB_HOVER) : clay_transparent()),
                .border = { .color = clay_html(THEME_BORDER), .width = { 0, 1, 0, 0, 0 } }
            })
            {
                Clay_String tab_name = { false, (int32_t)strlen(fake_files[tab_files[i]].name), fake_files[tab_files[i]].name };
                Clay__OpenTextElement(tab_name, (Clay_TextElementConfig){ .fontId = FONT_MONO, .fontSize = TERM_FONT_SIZE, .textColor = clay_html(is_active ? THEME_TEXT : THEME_TEXT_MUTED), .wrapMode = CLAY_TEXT_WRAP_NONE });

                if (Clay_Hovered() && clicked && !is_active)
                    select_file(tab_files[i]);
            }
        }
    }
}

// the code view of the selected file : one row per line, a fixed-width gutter
// holds the line number so the code column stays aligned
static void declare_code_area(void)
{
    const struct fake_file* file = &fake_files[selected_file];

    CLAY(CLAY_ID("code_scroll"),
    {
        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) }, .layoutDirection = CLAY_TOP_TO_BOTTOM, .padding = { 0, 0, 6, 0 } },
        .clip = { .vertical = true, .childOffset = Clay_GetScrollOffset() }
    })
    {
        for (uint32_t i = 0; i < file->num_lines; i++)
        {
            const char* line = file->lines[i];
            char number[8];
            snprintf(number, sizeof(number), "%u", i + 1);

            uint32_t line_color = THEME_TEXT;
            if ((line[0] == '/') && (line[1] == '/'))
                line_color = THEME_COMMENT;
            else if (line[0] == '#')
                line_color = THEME_ACCENT;

            CLAY(CLAY_IDI("code_line", i),
            {
                .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(LINE_PITCH) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .padding = { 8, 0, 0, 0 }, .childAlignment = { CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER } }
            })
            {
                CLAY(CLAY_IDI("code_gutter", i),
                {
                    .layout = { .sizing = { CLAY_SIZING_FIXED(36), CLAY_SIZING_FIT(0) } }
                })
                {
                    Clay_String num = { false, (int32_t)strlen(number), number };
                    Clay__OpenTextElement(num, (Clay_TextElementConfig){ .fontId = FONT_MONO, .fontSize = CODE_FONT_SIZE, .textColor = clay_html(THEME_LINENUM), .wrapMode = CLAY_TEXT_WRAP_NONE });
                }

                Clay_String code = { false, (int32_t)strlen(line), line };
                Clay__OpenTextElement(code, (Clay_TextElementConfig){ .fontId = FONT_MONO, .fontSize = CODE_FONT_SIZE, .textColor = clay_html(line_color), .wrapMode = CLAY_TEXT_WRAP_NONE });
            }
        }
    }
}

static void declare_terminal(bool clicked)
{
    CLAY(CLAY_ID("terminal"),
    {
        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(terminal_height) }, .layoutDirection = CLAY_TOP_TO_BOTTOM },
        .backgroundColor = clay_html(THEME_EDITOR)
    })
    {
        CLAY(CLAY_ID("terminal_header"),
        {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(26) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .padding = { 12, 8, 0, 0 }, .childGap = 8, .childAlignment = { CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER } },
            .backgroundColor = clay_html(THEME_TABBAR),
            .border = { .color = clay_html(THEME_BORDER), .width = { 0, 0, 0, 1, 0 } }
        })
        {
            CLAY_TEXT(CLAY_STRING("TERMINAL"),
            {
                .fontId = FONT_BOLD,
                .fontSize = SMALL_FONT_SIZE,
                .textColor = clay_html(THEME_TEXT_MUTED),
                .wrapMode = CLAY_TEXT_WRAP_NONE
            });

            // a transparent grow element pushes the button to the right edge
            CLAY(CLAY_ID("terminal_spacer"),
            {
                .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) } }
            })
            {}

            CLAY(CLAY_ID("terminal_clear"),
            {
                .layout = { .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIXED(18) }, .padding = { 10, 0, 0, 0 }, .childAlignment = { CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER } },
                .backgroundColor = Clay_Hovered() ? clay_html(THEME_BUTTON_HOVER) : clay_html(THEME_BUTTON),
                .cornerRadius = CLAY_CORNER_RADIUS(4)
            })
            {
                CLAY_TEXT(CLAY_STRING("Clear"),
                {
                    .fontId = FONT_UI,
                    .fontSize = TERM_FONT_SIZE,
                    .textColor = clay_html(THEME_TEXT),
                    .wrapMode = CLAY_TEXT_WRAP_NONE
                });

                if (Clay_Hovered() && clicked)
                {
                    terminal_lines = 0;
                    terminal_timer = 0.f;
                    terminal_follow = true;
                }
            }
        }

        CLAY(CLAY_ID("terminal_scroll"),
        {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) }, .layoutDirection = CLAY_TOP_TO_BOTTOM, .padding = { 12, 8, 6, 8 }, .childGap = 2 },
            .clip = { .vertical = true, .childOffset = Clay_GetScrollOffset() }
        })
        {
            for (uint32_t i = 0; i < terminal_lines; i++)
            {
                Clay_String line = { false, (int32_t)strlen(terminal_script[i].text), terminal_script[i].text };
                Clay__OpenTextElement(line, (Clay_TextElementConfig){ .fontId = FONT_MONO, .fontSize = TERM_FONT_SIZE, .textColor = clay_html(terminal_script[i].color), .wrapMode = CLAY_TEXT_WRAP_NONE });
            }
        }
    }
}

static void declare_status_bar(void)
{
    char line_count[32];
    snprintf(line_count, sizeof(line_count), "%u lines", fake_files[selected_file].num_lines);

    CLAY(CLAY_ID("status_bar"),
    {
        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(STATUS_BAR_HEIGHT) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .padding = { 12, 12, 0, 0 }, .childGap = 14, .childAlignment = { CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER } },
        .backgroundColor = clay_html(THEME_STATUS)
    })
    {
        CLAY_TEXT(CLAY_STRING("main"),
        {
            .fontId = FONT_UI,
            .fontSize = TERM_FONT_SIZE,
            .textColor = clay_html(0xffffffu),
            .wrapMode = CLAY_TEXT_WRAP_NONE
        });

        CLAY_TEXT(CLAY_STRING("Ln 1, Col 1"),
        {
            .fontId = FONT_UI,
            .fontSize = TERM_FONT_SIZE,
            .textColor = clay_html(0xffffffu),
            .wrapMode = CLAY_TEXT_WRAP_NONE
        });

        Clay_String count = { false, (int32_t)strlen(line_count), line_count };
        Clay__OpenTextElement(count, (Clay_TextElementConfig){ .fontId = FONT_UI, .fontSize = TERM_FONT_SIZE, .textColor = clay_html(0xffffffu), .wrapMode = CLAY_TEXT_WRAP_NONE });

        CLAY_TEXT(CLAY_STRING("UTF-8"),
        {
            .fontId = FONT_UI,
            .fontSize = TERM_FONT_SIZE,
            .textColor = clay_html(0xffffffu),
            .wrapMode = CLAY_TEXT_WRAP_NONE
        });

        CLAY_TEXT(CLAY_STRING("C"),
        {
            .fontId = FONT_UI,
            .fontSize = TERM_FONT_SIZE,
            .textColor = clay_html(0xffffffu),
            .wrapMode = CLAY_TEXT_WRAP_NONE
        });

        CLAY(CLAY_ID("status_spacer"),
        {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) } }
        })
        {}

        CLAY_TEXT(CLAY_STRING("Ready"),
        {
            .fontId = FONT_UI,
            .fontSize = TERM_FONT_SIZE,
            .textColor = clay_html(0xffffffu),
            .wrapMode = CLAY_TEXT_WRAP_NONE
        });
    }
}

static void declare_layout(void)
{
    Clay_PointerData pointer = Clay_GetPointerState();
    bool clicked = (pointer.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME);

    CLAY(CLAY_ID("root"),
    {
        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) }, .layoutDirection = CLAY_TOP_TO_BOTTOM }
    })
    {
        CLAY(CLAY_ID("main_row"),
        {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) }, .layoutDirection = CLAY_LEFT_TO_RIGHT }
        })
        {
            declare_sidebar(clicked);

            CLAY(CLAY_ID("splitter_v"),
            {
                .layout = { .sizing = { CLAY_SIZING_FIXED(SPLITTER_WIDTH), CLAY_SIZING_GROW(0) } },
                .backgroundColor = (Clay_Hovered() || (dragging_splitter == 1)) ? clay_html(THEME_ACCENT) : clay_html(THEME_BORDER)
            })
            {
                if (Clay_Hovered() && clicked)
                    dragging_splitter = 1;
            }

            CLAY(CLAY_ID("main_column"),
            {
                .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) }, .layoutDirection = CLAY_TOP_TO_BOTTOM }
            })
            {
                CLAY(CLAY_ID("editor"),
                {
                    .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) }, .layoutDirection = CLAY_TOP_TO_BOTTOM },
                    .backgroundColor = clay_html(THEME_EDITOR)
                })
                {
                    declare_tab_bar(clicked);
                    declare_code_area();
                }

                CLAY(CLAY_ID("splitter_h"),
                {
                    .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(SPLITTER_WIDTH) } },
                    .backgroundColor = (Clay_Hovered() || (dragging_splitter == 2)) ? clay_html(THEME_ACCENT) : clay_html(THEME_BORDER)
                })
                {
                    if (Clay_Hovered() && clicked)
                        dragging_splitter = 2;
                }

                declare_terminal(clicked);
            }
        }

        declare_status_bar();
    }
}

//----------------------------------------------------------------------------------------------------------------------------
// clay render commands -> onedraw

static float corner_radius_max(Clay_CornerRadius cr)
{
    float m = cr.topLeft;
    if (cr.topRight > m)
        m = cr.topRight;
    if (cr.bottomLeft > m)
        m = cr.bottomLeft;
    if (cr.bottomRight > m)
        m = cr.bottomRight;
    return m;
}

static void draw_tree_node(struct onedraw* r, Clay_BoundingBox bb, uint32_t node_index);

static void draw_clay_command(struct onedraw* r, Clay_RenderCommand* cmd)
{
    Clay_BoundingBox bb = cmd->boundingBox;

    switch (cmd->commandType)
    {
    case CLAY_RENDER_COMMAND_TYPE_RECTANGLE:
        od_draw_box(r, bb.x, bb.y, bb.x + bb.width, bb.y + bb.height, corner_radius_max(cmd->renderData.rectangle.cornerRadius), od_color(cmd->renderData.rectangle.backgroundColor));
        break;

    case CLAY_RENDER_COMMAND_TYPE_TEXT:
    {
        Clay_TextRenderData* t = &cmd->renderData.text;
        od_draw_string_view(r, bb.x, bb.y, t->stringContents.chars, (size_t)t->stringContents.length, font_from_id(t->fontId), (float)t->fontSize, od_color(t->textColor));
        break;
    }

    case CLAY_RENDER_COMMAND_TYPE_BORDER:
    {
        Clay_BorderRenderData* b = &cmd->renderData.border;
        draw_color color = od_color(b->color);
        if (b->width.top > 0)
            od_draw_box(r, bb.x, bb.y, bb.x + bb.width, bb.y + b->width.top, 0.f, color);
        if (b->width.bottom > 0)
            od_draw_box(r, bb.x, bb.y + bb.height - b->width.bottom, bb.x + bb.width, bb.y + bb.height, 0.f, color);
        if (b->width.left > 0)
            od_draw_box(r, bb.x, bb.y, bb.x + b->width.left, bb.y + bb.height, 0.f, color);
        if (b->width.right > 0)
            od_draw_box(r, bb.x + bb.width - b->width.right, bb.y, bb.x + bb.width, bb.y + bb.height, 0.f, color);
        break;
    }

    case CLAY_RENDER_COMMAND_TYPE_CUSTOM:
        draw_tree_node(r, bb, (uint32_t)(uintptr_t)cmd->renderData.custom.customData - 1);
        break;

    case CLAY_RENDER_COMMAND_TYPE_SCISSOR_START:
        od_canvas_cliprect(r, bb.x, bb.y, bb.x + bb.width, bb.y + bb.height);
        break;

    case CLAY_RENDER_COMMAND_TYPE_SCISSOR_END:
        od_pop_clip(r);
        break;

    default:
        break;
    }
}

// The tree nodes are CUSTOM clay elements : clay owns their position and size, onedraw owns the pixels
static void draw_tree_node(struct onedraw* r, Clay_BoundingBox bb, uint32_t node_index)
{
    struct tree_entry* e = &tree[node_index];
    const float indent = 14.f;
    float cy = bb.y + bb.height * 0.5f;

    bool selected = (!e->is_folder) && (e->file_index == selected_file);
    bool hovered = Clay_PointerOver(CLAY_IDI("tree_entry", node_index));
    if (selected)
        od_draw_box(r, bb.x, bb.y, bb.x + bb.width, bb.y + bb.height, 0.f, od_html(THEME_SELECTED));
    else if (hovered)
        od_draw_box(r, bb.x, bb.y, bb.x + bb.width, bb.y + bb.height, 0.f, od_html(THEME_HOVER));

    // one indent guide per ancestor level
    for (uint32_t level = 0; level < e->depth; level++)
    {
        float gx = bb.x + level * indent + indent * 0.5f;
        od_draw_line(r, gx, bb.y + 3.f, gx, bb.y + bb.height - 3.f, 1.f, od_html(THEME_GUIDE));
    }

    float base_x = bb.x + e->depth * indent;

    if (e->is_folder)
    {
        float tri[6];
        if (e->expanded)   // pointing down
        {
            tri[0] = base_x + 2.f; tri[1] = cy - 2.f;
            tri[2] = base_x + 10.f; tri[3] = cy - 2.f;
            tri[4] = base_x + 6.f; tri[5] = cy + 3.f;
        }
        else               // pointing right
        {
            tri[0] = base_x + 3.f; tri[1] = cy - 4.f;
            tri[2] = base_x + 3.f; tri[3] = cy + 4.f;
            tri[4] = base_x + 8.f; tri[5] = cy;
        }
        od_draw_triangle(r, tri, 0.5f, od_html(THEME_TEXT_MUTED));

        // folder : tab + body
        od_draw_box(r, base_x + 14.f, cy - 5.f, base_x + 20.f, cy - 2.f, 1.f, od_html(THEME_GLYPH_FOLDER));
        od_draw_box(r, base_x + 14.f, cy - 3.f, base_x + 27.f, cy + 4.f, 1.5f, od_html(THEME_GLYPH_FOLDER));
    }
    else
    {
        uint32_t file_color = THEME_GLYPH_FILE;
        const char* name = e->label;
        size_t len = strlen(name);
        if (len >= 2 && strcmp(name + len - 2, ".c") == 0)
            file_color = THEME_GLYPH_FILE_C;
        else if (len >= 3 && strcmp(name + len - 3, ".md") == 0)
            file_color = THEME_GLYPH_FILE_MD;
        else if (strncmp(name, "CMake", 5) == 0)
            file_color = THEME_GLYPH_FILE_CMAKE;

        // file : body + text lines
        od_draw_box(r, base_x + 15.f, cy - 6.f, base_x + 25.f, cy + 6.f, 1.5f, od_html(file_color));
        od_draw_line(r, base_x + 17.5f, cy - 2.5f, base_x + 22.5f, cy - 2.5f, 1.f, 0xffffffff);
        od_draw_line(r, base_x + 17.5f, cy + 0.5f, base_x + 22.5f, cy + 0.5f, 1.f, 0xffffffff);
        od_draw_line(r, base_x + 17.5f, cy + 3.5f, base_x + 20.5f, cy + 3.5f, 1.f, 0xffffffff);
    }

    float line_h = od_text_line_height(r, od_font_regular, (float)UI_FONT_SIZE);
    draw_color label_color = e->is_folder ? od_html(THEME_TEXT_MUTED) : od_html(THEME_TEXT);
    od_draw_text(r, base_x + 32.f, bb.y + (bb.height - line_h) * 0.5f, e->label, od_font_regular, (float)UI_FONT_SIZE, label_color);
}

static bool point_in_box(float x, float y, Clay_BoundingBox b)
{
    return (x >= b.x) && (x <= b.x + b.width) && (y >= b.y) && (y <= b.y + b.height);
}

// clay has no scrollbar of its own : the app reads the scroll container data after layout,
// draws a thumb with onedraw and writes back to the scroll position while dragging (clay picks
// it up through Clay_GetScrollOffset() on the next frame)
static void draw_scrollbars(struct onedraw* r, float px, float py)
{
    const Clay_ElementId ids[2] = { CLAY_ID("code_scroll"), CLAY_ID("terminal_scroll") };
    const float track_w = 10.f;
    const float thumb_w = 8.f;
    const float thumb_min_h = 24.f;
    bool any_dragging = scrollbars[0].dragging || scrollbars[1].dragging;

    for (uint32_t i = 0; i < 2; i++)
    {
        struct scrollbar* sb = &scrollbars[i];
        sb->thumb = (Clay_BoundingBox){ 0.f, 0.f, 0.f, 0.f };

        Clay_ElementData ed = Clay_GetElementData(ids[i]);
        Clay_ScrollContainerData sd = Clay_GetScrollContainerData(ids[i]);
        if (!ed.found || !sd.found)
            continue;

        Clay_BoundingBox bb = ed.boundingBox;
        float max_scroll = sd.contentDimensions.height - sd.scrollContainerDimensions.height;
        if (max_scroll <= 0.f)
            continue;   // the content fits, no scrollbar needed

        float thumb_h = CLAMP(bb.height * (bb.height / sd.contentDimensions.height), thumb_min_h, bb.height);
        float ratio = CLAMP(-(*sd.scrollPosition).y / max_scroll, 0.f, 1.f);
        float thumb_y = bb.y + ratio * (bb.height - thumb_h);
        sb->thumb = (Clay_BoundingBox){ bb.x + bb.width - track_w + (track_w - thumb_w) * 0.5f, thumb_y, thumb_w, thumb_h };

        if (!sb->dragging && !any_dragging && (dragging_splitter == 0) && mouse_down && point_in_box(px, py, sb->thumb))
        {
            sb->dragging = true;
            sb->grab_offset = py - thumb_y;
            if (i == 1)
                terminal_follow = false;
        }

        if (sb->dragging)
        {
            if (!mouse_down)
            {
                sb->dragging = false;
            }
            else
            {
                float t = CLAMP((py - sb->grab_offset - bb.y) / (bb.height - thumb_h), 0.f, 1.f);
                (*sd.scrollPosition).y = -t * max_scroll;
            }
        }

        draw_color color = sb->dragging ? od_html(THEME_SCROLLBAR_DRAG) : od_html(THEME_SCROLLBAR);
        od_draw_box(r, sb->thumb.x, sb->thumb.y, sb->thumb.x + sb->thumb.width, sb->thumb.y + sb->thumb.height, 4.f, color);
    }
}

//----------------------------------------------------------------------------------------------------------------------------
// sokol_app callbacks

static void frame(void)
{
    if (renderer == NULL)
        return;

    // fully obscured window : macOS provides no drawable, skip the frame until visible
    sapp_swapchain sc = sapp_acquire_swapchain();
    if (sc.invalid)
        return;

    float dt = frame_delta_time();
    if (dt > 0.25f)
        dt = 0.25f;

    // sapp_width() is the framebuffer size in device pixels : divide by the dpi scale to get
    // window points, the canvas maps those points onto the framebuffer (the scale is applied by
    // the canvas transform, so the UI keeps its size and sharpness on retina displays)
    float scale = sapp_dpi_scale();
    float width = (float)sapp_width() / scale;
    float height = (float)sapp_height() / scale;

    // dragging a splitter sets the pane size directly from the pointer position
    if (dragging_splitter == 1)
        sidebar_width = CLAMP(mouse_x / scale, 120.f, width * 0.6f);
    if (dragging_splitter == 2)
        terminal_height = CLAMP(height - STATUS_BAR_HEIGHT - (mouse_y / scale), 80.f, height * 0.7f);
    if (!mouse_down)
        dragging_splitter = 0;

    // logical-points canvas : clay works in the same units
    od_set_canvas(renderer, 0.f, 0.f, width, height);
    Clay_SetLayoutDimensions((Clay_Dimensions){ width, height });

    // sokol reports the mouse in device pixels, clay works in the canvas's logical points
    Clay_SetPointerState((Clay_Vector2){ mouse_x / scale, mouse_y / scale }, mouse_down);

    // AppKit inverts the scroll delta when "natural scrolling" (the default) is on :
    // scroll_y > 0 moves the content up, which is a negative offset in clay's
    // [-(content - container); 0] scroll range
    bool wheel_this_frame = (pending_scroll.x != 0.f) || (pending_scroll.y != 0.f);
    Clay_Vector2 scroll_delta = { -pending_scroll.x, -pending_scroll.y };
    Clay_UpdateScrollContainers(false, scroll_delta, dt);
    pending_scroll = (Clay_Vector2){ 0.f, 0.f };

    update_terminal(dt);

    od_begin_frame(renderer);

    Clay_BeginLayout();
    declare_layout();
    Clay_RenderCommandArray commands = Clay_EndLayout(dt);
    for (int32_t i = 0; i < commands.length; i++)
        draw_clay_command(renderer, Clay_RenderCommandArray_Get(&commands, i));

    draw_scrollbars(renderer, mouse_x / scale, mouse_y / scale);

    od_end_frame(renderer, (void*)sc.metal.current_drawable);

    // the internal scroll positions are read back by Clay_GetScrollOffset() during the next
    // frame's element declaration, so fixes applied here take effect one frame later
    if (code_scroll_to_top)
    {
        Clay_ScrollContainerData code = Clay_GetScrollContainerData(CLAY_ID("code_scroll"));
        if (code.found)
            *code.scrollPosition = (Clay_Vector2){ 0.f, 0.f };
        code_scroll_to_top = false;
    }

    // the terminal pins itself to its tail while following : the user stops the follow by
    // wheeling inside the terminal or dragging its scrollbar
    Clay_ElementData term_box = Clay_GetElementData(CLAY_ID("terminal_scroll"));
    if ((terminal_follow) && wheel_this_frame && term_box.found &&
        point_in_box(mouse_x / scale, mouse_y / scale, term_box.boundingBox))
    {
        terminal_follow = false;
    }

    if (terminal_follow)
    {
        Clay_ScrollContainerData term = Clay_GetScrollContainerData(CLAY_ID("terminal_scroll"));
        if (term.found)
        {
            float max_scroll = CLAMP(term.contentDimensions.height - term.scrollContainerDimensions.height, 0.f, 1e9f);
            (*term.scrollPosition).y = -max_scroll;
        }
    }
}

static void event(const sapp_event* e)
{
    if (renderer == NULL)
        return;

    // the event struct carries the current mouse position in every event
    mouse_x = e->mouse_x;
    mouse_y = e->mouse_y;

    switch (e->type)
    {
    case SAPP_EVENTTYPE_RESIZED:
        od_resize(renderer, (uint32_t)e->framebuffer_width, (uint32_t)e->framebuffer_height);
        break;

    case SAPP_EVENTTYPE_MOUSE_DOWN:
        mouse_down = true;
        break;

    case SAPP_EVENTTYPE_MOUSE_UP:
        mouse_down = false;
        break;

    case SAPP_EVENTTYPE_MOUSE_SCROLL:
        pending_scroll.x += e->scroll_x;
        pending_scroll.y += e->scroll_y;
        break;

    case SAPP_EVENTTYPE_KEY_DOWN:
        if ((e->key_code == SAPP_KEYCODE_Q) && (e->modifiers & SAPP_MODIFIER_SUPER))
            sapp_quit();
        break;

    default:
        break;
    }
}

static void init(void)
{
    sapp_environment env = sapp_get_environment();

    // +32 : linear_alloc rounds each allocation up to a 16-byte boundary
    size_t total = od_min_memory_size() + (size_t)Clay_MinMemorySize() + 32;
    if (!linear_init(&memory, total))
        return;

    void* od_buffer = linear_alloc(&memory, od_min_memory_size());
    renderer = od_init( &(onedraw_def)
    {
        .metal_device = (void*)env.metal.device,
        .preallocated_buffer = od_buffer,
        .viewport_width = (uint32_t)sapp_width(),
        .viewport_height = (uint32_t)sapp_height(),
        .log_func = custom_log,
        .srgb_backbuffer = false
    });

    if (renderer == NULL)
        return;

    od_set_clear_color(renderer, od_html(THEME_SIDEBAR));

    void* clay_memory = linear_alloc(&memory, Clay_MinMemorySize());
    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(Clay_MinMemorySize(), clay_memory);

    float scale = sapp_dpi_scale();
    Clay_Initialize(arena, (Clay_Dimensions){ (float)sapp_width() / scale, (float)sapp_height() / scale }, (Clay_ErrorHandler){ .errorHandlerFunction = clay_error, .userData = NULL });
    Clay_SetMeasureTextFunction(measure_text, renderer);
}

static void cleanup(void)
{
    if (renderer != NULL)
    {
        od_terminate(renderer);
        renderer = NULL;
    }
    linear_free(&memory);
}

sapp_desc sokol_main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;

    return (sapp_desc)
    {
        .width = 1280,
        .height = 720,
        .init_cb = init,
        .frame_cb = frame,
        .cleanup_cb = cleanup,
        .event_cb = event,
        .high_dpi = true,
        .window_title = "clay example"
    };
}
