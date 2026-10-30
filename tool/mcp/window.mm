#import <AppKit/AppKit.h>

// The vendored sokol_app has no programmatic window resize API. This changes the window's
// point size; sokol's existing windowDidResize: pipeline does the rest (recompute dpi_scale,
// resize the Metal framebuffer, emit SAPP_EVENTTYPE_RESIZED)
extern "C" void mcp_set_window_points(int w, int h)
{
    NSWindow* window = [NSApp mainWindow];
    if (window == nil)
        return;
    [window setContentSize: NSMakeSize((CGFloat) w, (CGFloat) h)];
}
