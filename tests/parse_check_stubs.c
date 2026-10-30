#include "../lib/onedraw.h"

// Empty onedraw primitives so the scene / parser can be exercised headless (no GPU)
void od_draw_disc(struct onedraw* r, float a, float b, float c, draw_color d){(void)r;(void)a;(void)b;(void)c;(void)d;}
void od_draw_ring(struct onedraw* r, float a, float b, float c, float d, draw_color e){(void)r;(void)a;(void)b;(void)c;(void)d;(void)e;}
void od_draw_disc_gradient(struct onedraw* r, float a, float b, float c, draw_color d, draw_color e){(void)r;(void)a;(void)b;(void)c;(void)d;(void)e;}
void od_draw_box(struct onedraw* r, float a, float b, float c, float d, float e, draw_color f){(void)r;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;}
void od_draw_blurred_box(struct onedraw* r, float a, float b, float c, float d, float e, draw_color f){(void)r;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;}
void od_draw_line(struct onedraw* r, float a, float b, float c, float d, float e, draw_color f){(void)r;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;}
void od_draw_capsule(struct onedraw* r, float a, float b, float c, float d, float e, draw_color f){(void)r;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;}
void od_draw_capsule_gradient(struct onedraw* r, float a, float b, float c, float d, float e, draw_color f, draw_color g){(void)r;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;}
void od_draw_oriented_box(struct onedraw* r, float a, float b, float c, float d, float e, float f, draw_color g){(void)r;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;}
void od_draw_oriented_rect(struct onedraw* r, float a, float b, float c, float d, float e, float f, float g, draw_color h){(void)r;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;}
void od_draw_ellipse(struct onedraw* r, float a, float b, float c, float d, float e, draw_color f){(void)r;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;}
void od_draw_ellipse_ring(struct onedraw* r, float a, float b, float c, float d, float e, float f, draw_color g){(void)r;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;}
void od_draw_triangle(struct onedraw* r, const float* v, float x, draw_color c){(void)r;(void)v;(void)x;(void)c;}
void od_draw_triangle_ring(struct onedraw* r, const float* v, float x, float y, draw_color c){(void)r;(void)v;(void)x;(void)y;(void)c;}
void od_draw_sector(struct onedraw* r, float a, float b, float c, float d, float e, draw_color f){(void)r;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;}
void od_draw_sector_ring(struct onedraw* r, float a, float b, float c, float d, float e, float f, draw_color g){(void)r;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;}
void od_draw_arc(struct onedraw* r, float a, float b, float c, float d, float e, float f, float g, draw_color h){(void)r;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;}
uint32_t od_draw_quadratic_bezier(struct onedraw* r, const float* v, float w, draw_color c){(void)r;(void)v;(void)w;(void)c;return 0;}
uint32_t od_draw_cubic_bezier(struct onedraw* r, const float* v, float w, draw_color c){(void)r;(void)v;(void)w;(void)c;return 0;}
void od_draw_char(struct onedraw* r, float x, float y, char ch, draw_color c){(void)r;(void)x;(void)y;(void)ch;(void)c;}
void od_draw_text(struct onedraw* r, float x, float y, const char* t, draw_color c){(void)r;(void)x;(void)y;(void)t;(void)c;}
uint32_t od_draw_dashed_line(struct onedraw* r, float a, float b, float c, float d, float e, od_dash_style s, draw_color f){(void)r;(void)a;(void)b;(void)c;(void)d;(void)e;(void)s;(void)f;return 0;}
uint32_t od_draw_dashed_ring(struct onedraw* r, float a, float b, float c, float d, od_dash_style s, draw_color e){(void)r;(void)a;(void)b;(void)c;(void)d;(void)s;(void)e;return 0;}
uint32_t od_draw_dashed_box(struct onedraw* r, float a, float b, float c, float d, float e, float f, od_dash_style s, draw_color g){(void)r;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)s;(void)g;return 0;}
uint32_t od_draw_dashed_oriented_box(struct onedraw* r, float a, float b, float c, float d, float e, float f, float g, od_dash_style s, draw_color h){(void)r;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)s;(void)h;return 0;}
uint32_t od_draw_dashed_ellipse_ring(struct onedraw* r, float a, float b, float c, float d, float e, float f, od_dash_style s, draw_color g){(void)r;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)s;(void)g;return 0;}
uint32_t od_draw_dashed_triangle_ring(struct onedraw* r, const float* v, float w, od_dash_style s, draw_color c){(void)r;(void)v;(void)w;(void)s;(void)c;return 0;}
void od_begin_group(struct onedraw* r, bool smooth, float value, float width){(void)r;(void)smooth;(void)value;(void)width;}
void od_end_group(struct onedraw* r, draw_color c){(void)r;(void)c;}
void od_push_cliprect(struct onedraw* r, float a, float b, float c, float d){(void)r;(void)a;(void)b;(void)c;(void)d;}
void od_push_cliprect_rounded(struct onedraw* r, float a, float b, float c, float d, float e){(void)r;(void)a;(void)b;(void)c;(void)d;(void)e;}
void od_push_clipdisc(struct onedraw* r, float a, float b, float c){(void)r;(void)a;(void)b;(void)c;}
void od_pop_clip(struct onedraw* r){(void)r;}
