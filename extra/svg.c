#include <math.h>
#include <string.h>
#include <ctype.h>

#include "../lib/onedraw.h"
#include "svg.h"

#define SV_IMPLEMENTATION
#include <sv.h>

#define SVG_INVALID (~0u)

enum svg_el_type
{
    svg_el_g,
    svg_el_defs,
    svg_el_symbol,
    svg_el_use,
    svg_el_svg,
    svg_el_rect,
    svg_el_circle,
    svg_el_ellipse,
    svg_el_line,
    svg_el_polygon,
    svg_el_polyline,
    svg_el_path,
    svg_el_text,
    svg_el_tspan,
    svg_el_clip_path,
    svg_el_image,
    svg_el_unknown
};

enum
{
    svg_set_color = 1 << 0,
    svg_set_fill = 1 << 1,
    svg_set_stroke = 1 << 2,
    svg_set_opacity = 1 << 3,
    svg_set_fill_opacity = 1 << 4,
    svg_set_stroke_opacity = 1 << 5,
    svg_set_stroke_width = 1 << 6,
    svg_set_linecap = 1 << 7,
    svg_set_linejoin = 1 << 8,
    svg_set_miterlimit = 1 << 9,
    svg_set_dash = 1 << 10,
    svg_set_dash_offset = 1 << 11,
    svg_set_font = 1 << 12,
    svg_set_font_size = 1 << 13,
    svg_set_anchor = 1 << 14,
    svg_set_display = 1 << 15,
    svg_set_visibility = 1 << 16
};

typedef struct svg_pt
{
    float x, y;
} svg_pt;

typedef struct svg_xform
{
    float a, b, c, d, e, f;
} svg_xform;

typedef struct svg_style
{
    uint32_t color;
    uint32_t fill;
    bool fill_none;
    bool fill_current;
    uint32_t stroke;
    bool stroke_none;
    bool stroke_current;
    float opacity;
    float fill_opacity;
    float stroke_opacity;
    float stroke_width;
    float miterlimit;
    float dash_offset;
    float font_size;
    float dash[2];
    uint8_t linecap;
    uint8_t linejoin;
    uint8_t num_dash;
    uint8_t dash_set;
    uint8_t font;
    uint8_t anchor;
    uint8_t display;
    uint8_t visibility;
} svg_style;

enum svg_path_op
{
    svg_path_move,
    svg_path_line,
    svg_path_hline,
    svg_path_vline,
    svg_path_cubic,
    svg_path_quadratic,
    svg_path_arc,
    svg_path_close
};

typedef struct svg_path_cmd
{
    uint8_t op;
    uint8_t count;
    float v[7];
} svg_path_cmd;

typedef struct svg_rule
{
    uint32_t str_off;
    uint32_t str_len;
    uint32_t spec;
    uint32_t mask;
    svg_style props;
} svg_rule;

// per-element params layout, by type
//   rect        : x, y, w, h, rx, ry
//   circle      : cx, cy, r
//   ellipse     : cx, cy, rx, ry
//   line        : x1, y1, x2, y2
//   use         : x, y, w, h
//   image       : x, y, w, h
//   svg/symbol  : x, y, w, h, minx, miny, vbw, vbh, par_mode, par_ax, par_ay
//   text        : x, y, mask (bit0 x set, bit1 y set)
//   tspan       : x, y, dx, dy, mask (bit0 x, bit1 y, bit2 dx, bit3 dy)
typedef struct svg_element
{
    uint32_t type;
    uint32_t first_child;
    uint32_t next_sibling;
    uint32_t parent;
    uint32_t params;
    uint32_t num_params;
    uint32_t param_mask;
    uint32_t points;
    uint32_t num_points;
    uint32_t path;
    uint32_t num_path;
    uint32_t text;
    uint32_t text_len;
    uint32_t transform;
    uint32_t name_off;
    uint32_t name_len;
    uint32_t class_off;
    uint32_t class_len;
    uint32_t id;
    uint32_t href_off;
    uint32_t href_len;
    uint32_t clip_off;
    uint32_t clip_len;
} svg_element;

struct svg_document
{
    svg_allocator alloc;
    uint32_t flags;
    uint32_t root;
    float root_width;
    float root_height;
    svg_element* elements;
    uint32_t num_elements;
    svg_style* pres;
    uint32_t* pres_mask;
    svg_style* inl;
    uint32_t* inl_mask;
    float* params;
    uint32_t num_params;
    float* points;
    uint32_t num_points;
    svg_path_cmd* paths;
    uint32_t num_paths;
    char* text;
    uint32_t num_text;
    float* transforms;
    char* strings;
    uint32_t num_strings;
    char* rule_strs;
    uint32_t num_rule_strs;
    svg_rule* rules;
    uint32_t num_rules;
    uint32_t* id_elems;
    uint32_t* id_off;
    uint32_t* id_len;
    uint32_t num_ids;
};

typedef struct svg_mem
{
    svg_allocator alloc;
} svg_mem;

static void* svg_mem_alloc(svg_mem* m, size_t size, size_t alignment)
{
    (void) alignment;
    return m->alloc.alloc ? m->alloc.alloc(m->alloc.user_data, size, alignment) : NULL;
}

static void svg_mem_free(svg_mem* m, void* p)
{
    if (m->alloc.free && p != NULL)
        m->alloc.free(m->alloc.user_data, p);
}

static svg_style svg_style_defaults(void)
{
    svg_style s = {0};
    s.color = 0xff000000;
    s.fill = 0xff000000;
    s.stroke_none = true;
    s.opacity = 1.f;
    s.fill_opacity = 1.f;
    s.stroke_opacity = 1.f;
    s.stroke_width = 1.f;
    s.miterlimit = 4.f;
    s.font = od_font_regular;
    s.font_size = 16.f;
    return s;
}

static svg_xform svg_xform_identity(void)
{
    svg_xform m = {1.f, 0.f, 0.f, 1.f, 0.f, 0.f};
    return m;
}

// out = m1 * m2, m2 applied first
static svg_xform svg_xform_mul(const svg_xform* m1, const svg_xform* m2)
{
    svg_xform r;
    r.a = m1->a * m2->a + m1->c * m2->b;
    r.b = m1->b * m2->a + m1->d * m2->b;
    r.c = m1->a * m2->c + m1->c * m2->d;
    r.d = m1->b * m2->c + m1->d * m2->d;
    r.e = m1->a * m2->e + m1->c * m2->f + m1->e;
    r.f = m1->b * m2->e + m1->d * m2->f + m1->f;
    return r;
}

static svg_pt svg_xform_pt(const svg_xform* m, float x, float y)
{
    svg_pt p;
    p.x = m->a * x + m->c * y + m->e;
    p.y = m->b * x + m->d * y + m->f;
    return p;
}

// 0 = identity, 1 = translation + uniform scale + rotation, 2 = general
static uint32_t svg_xform_classify(const svg_xform* m)
{
    if (m->a == 1.f && m->d == 1.f && m->b == 0.f && m->c == 0.f)
        return 0;
    if (m->a == m->d && m->b == -m->c)
        return 1;
    return 2;
}

static float svg_xform_uniform_scale(const svg_xform* m)
{
    return sqrtf(m->a * m->a + m->b * m->b);
}

//----------------------------------------------------------------------------------------------------------------------------
// colors

static const struct
{
    const char* name;
    uint8_t r, g, b;
} svg_named_colors[] =
{
    {"aliceblue", 240, 248, 255}, {"antiquewhite", 250, 235, 215}, {"aqua", 0, 255, 255},
    {"aquamarine", 127, 255, 212}, {"azure", 240, 255, 255}, {"beige", 245, 245, 220},
    {"bisque", 255, 228, 196}, {"black", 0, 0, 0}, {"blanchedalmond", 255, 235, 205},
    {"blue", 0, 0, 255}, {"blueviolet", 138, 43, 226}, {"brown", 165, 42, 42},
    {"burlywood", 222, 184, 135}, {"cadetblue", 95, 158, 160}, {"chartreuse", 127, 255, 0},
    {"chocolate", 210, 105, 30}, {"coral", 255, 127, 80}, {"cornflowerblue", 100, 149, 237},
    {"cornsilk", 255, 248, 220}, {"crimson", 220, 20, 60}, {"cyan", 0, 255, 255},
    {"darkblue", 0, 0, 139}, {"darkcyan", 0, 139, 139}, {"darkgoldenrod", 184, 134, 11},
    {"darkgray", 169, 169, 169}, {"darkgreen", 0, 100, 0}, {"darkgrey", 169, 169, 169},
    {"darkkhaki", 189, 183, 107}, {"darkmagenta", 139, 0, 139}, {"darkolivegreen", 85, 107, 47},
    {"darkorange", 255, 140, 0}, {"darkorchid", 153, 50, 204}, {"darkred", 139, 0, 0},
    {"darksalmon", 233, 150, 122}, {"darkseagreen", 143, 188, 143}, {"darkslateblue", 72, 61, 139},
    {"darkslategray", 47, 79, 79}, {"darkslategrey", 47, 79, 79}, {"darkturquoise", 0, 206, 209},
    {"darkviolet", 148, 0, 211}, {"deeppink", 255, 20, 147}, {"deepskyblue", 0, 191, 255},
    {"dimgray", 105, 105, 105}, {"dimgrey", 105, 105, 105}, {"dodgerblue", 30, 144, 255},
    {"firebrick", 178, 34, 34}, {"floralwhite", 255, 250, 240}, {"forestgreen", 34, 139, 34},
    {"fuchsia", 255, 0, 255}, {"gainsboro", 220, 220, 220}, {"ghostwhite", 248, 248, 255},
    {"gold", 255, 215, 0}, {"goldenrod", 218, 165, 32}, {"gray", 128, 128, 128},
    {"green", 0, 128, 0}, {"greenyellow", 173, 255, 47}, {"grey", 128, 128, 128},
    {"honeydew", 240, 255, 240}, {"hotpink", 255, 105, 180}, {"indianred", 205, 92, 92},
    {"indigo", 75, 0, 130}, {"ivory", 255, 255, 240}, {"khaki", 240, 230, 140},
    {"lavender", 230, 230, 250}, {"lavenderblush", 255, 240, 245}, {"lawngreen", 124, 252, 0},
    {"lemonchiffon", 255, 250, 205}, {"lightblue", 173, 216, 230}, {"lightcoral", 240, 128, 128},
    {"lightcyan", 224, 255, 255}, {"lightgoldenrodyellow", 250, 250, 210}, {"lightgray", 211, 211, 211},
    {"lightgreen", 144, 238, 144}, {"lightgrey", 211, 211, 211}, {"lightpink", 255, 182, 193},
    {"lightsalmon", 255, 160, 122}, {"lightseagreen", 32, 178, 170}, {"lightskyblue", 135, 206, 250},
    {"lightslategray", 119, 136, 153}, {"lightslategrey", 119, 136, 153}, {"lightsteelblue", 176, 196, 222},
    {"lightyellow", 255, 255, 224}, {"lime", 0, 255, 0}, {"limegreen", 50, 205, 50},
    {"linen", 250, 240, 230}, {"magenta", 255, 0, 255}, {"maroon", 128, 0, 0},
    {"mediumaquamarine", 102, 205, 170}, {"mediumblue", 0, 0, 205}, {"mediumorchid", 186, 85, 211},
    {"mediumpurple", 147, 112, 219}, {"mediumseagreen", 60, 179, 113}, {"mediumslateblue", 123, 104, 238},
    {"mediumspringgreen", 0, 250, 154}, {"mediumturquoise", 72, 209, 204}, {"mediumvioletred", 199, 21, 133},
    {"midnightblue", 25, 25, 112}, {"mintcream", 245, 255, 250}, {"mistyrose", 255, 228, 225},
    {"moccasin", 255, 228, 181}, {"navajowhite", 255, 222, 173}, {"navy", 0, 0, 128},
    {"oldlace", 253, 245, 230}, {"olive", 128, 128, 0}, {"olivedrab", 107, 142, 35},
    {"orange", 255, 165, 0}, {"orangered", 255, 69, 0}, {"orchid", 218, 112, 214},
    {"palegoldenrod", 238, 232, 170}, {"palegreen", 152, 251, 152}, {"paleturquoise", 175, 238, 238},
    {"palevioletred", 219, 112, 147}, {"papayawhip", 255, 239, 213}, {"peachpuff", 255, 218, 185},
    {"peru", 205, 133, 63}, {"pink", 255, 192, 203}, {"plum", 221, 160, 221},
    {"powderblue", 176, 224, 230}, {"purple", 128, 0, 128}, {"rebeccapurple", 102, 51, 153},
    {"red", 255, 0, 0}, {"rosybrown", 188, 143, 143}, {"royalblue", 65, 105, 225},
    {"saddlebrown", 139, 69, 19}, {"salmon", 250, 128, 114}, {"sandybrown", 244, 164, 96},
    {"seagreen", 46, 139, 87}, {"seashell", 255, 245, 238}, {"sienna", 160, 82, 45},
    {"silver", 192, 192, 192}, {"skyblue", 135, 206, 235}, {"slateblue", 106, 90, 205},
    {"slategray", 112, 128, 144}, {"slategrey", 112, 128, 144}, {"snow", 255, 250, 250},
    {"springgreen", 0, 255, 127}, {"steelblue", 70, 130, 180}, {"tan", 210, 180, 140},
    {"teal", 0, 128, 128}, {"thistle", 216, 191, 216}, {"tomato", 255, 99, 71},
    {"turquoise", 64, 224, 208}, {"violet", 238, 130, 238}, {"wheat", 245, 222, 179},
    {"white", 255, 255, 255}, {"whitesmoke", 245, 245, 245}, {"yellow", 255, 255, 0},
    {"yellowgreen", 154, 205, 50}
};

static uint32_t svg_color_pack(float r, float g, float b, float a)
{
    uint32_t vr = (uint32_t)(r * 255.f + .5f);
    uint32_t vg = (uint32_t)(g * 255.f + .5f);
    uint32_t vb = (uint32_t)(b * 255.f + .5f);
    uint32_t va = (uint32_t)(a * 255.f + .5f);
    if (vr > 255) vr = 255;
    if (vg > 255) vg = 255;
    if (vb > 255) vb = 255;
    if (va > 255) va = 255;
    return (va << 24) | (vb << 16) | (vg << 8) | vr;
}

static uint32_t svg_color_alpha(uint32_t color, float a)
{
    uint32_t va = (uint32_t)((float)((color >> 24) & 0xff) * a + .5f);
    if (va > 255)
        va = 255;
    return (color & 0x00ffffff) | (va << 24);
}

static bool svg_hex4(String_View sv, uint32_t* v)
{
    for (uint32_t i = 0; i < sv.count; ++i)
    {
        char c = sv.data[i];
        uint32_t d;
        if (c >= '0' && c <= '9')
            d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f')
            d = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            d = (uint32_t)(c - 'A' + 10);
        else
            return false;
        *v = (*v << 4) | d;
    }
    return true;
}

static bool svg_parse_number(String_View sv, size_t* i, float* out);

// parses a color value ; [out_none] / [out_current] for "none" / "currentColor"
static bool svg_parse_color(String_View sv, uint32_t* out, bool* out_none, bool* out_current)
{
    sv = sv_trim(sv);
    *out_none = false;
    *out_current = false;

    if (sv_eq_ignorecase(sv, SV("none")))
    {
        *out_none = true;
        return true;
    }

    if (sv_eq_ignorecase(sv, SV("currentColor")))
    {
        *out_current = true;
        return true;
    }

    if (sv.count > 0 && sv.data[0] == '#')
    {
        String_View hex = sv_from_parts(sv.data + 1, sv.count - 1);
        uint32_t v = 0;
        if (hex.count == 3 && svg_hex4(hex, &v))
        {
            *out = svg_color_pack((float)((v >> 8) & 15) * 17.f / 255.f, (float)((v >> 4) & 15) * 17.f / 255.f,
                                  (float)(v & 15) * 17.f / 255.f, 1.f);
            return true;
        }
        if (hex.count == 6 && svg_hex4(hex, &v))
        {
            *out = svg_color_pack((float)((v >> 16) & 255) / 255.f, (float)((v >> 8) & 255) / 255.f, (float)(v & 255) / 255.f, 1.f);
            return true;
        }
        if (hex.count == 8 && svg_hex4(hex, &v))
        {
            *out = svg_color_pack((float)((v >> 24) & 255) / 255.f, (float)((v >> 16) & 255) / 255.f,
                                  (float)((v >> 8) & 255) / 255.f, (float)(v & 255) / 255.f);
            return true;
        }
        return false;
    }

    if (sv_starts_with(sv, SV("rgb")) || sv_starts_with(sv, SV("rgba")))
    {
        String_View rest = sv_from_parts(sv.data + 3, sv.count - 3);
        if (rest.count < 2 || rest.data[0] != '(' || !sv_ends_with(rest, SV(")")))
            return false;
        rest = sv_from_parts(rest.data + 1, rest.count - 2);

        float channel[3] = {0.f, 0.f, 0.f};
        float alpha = 1.f;
        uint32_t n = 0;
        while (rest.count > 0 && n < 4)
        {
            rest = sv_trim_left(rest);
            if (rest.count == 0)
                break;
            bool percent = rest.count > 0 && rest.data[rest.count - 1] == '%';
            if (percent)
                rest = sv_from_parts(rest.data, rest.count - 1);
            rest = sv_trim_right(rest);
            size_t i = 0;
            float v = 0.f;
            if (!svg_parse_number(rest, &i, &v))
                return false;
            if (percent)
                v *= 255.f / 100.f;
            if (n < 3)
                channel[n] = v;
            else
                alpha = v <= 1.f ? v : v / 255.f;
            ++n;
            rest = sv_from_parts(rest.data + i, rest.count - i);
            if (rest.count > 0 && rest.data[0] == ',')
                rest = sv_from_parts(rest.data + 1, rest.count - 1);
        }
        if (n < 3)
            return false;
        *out = svg_color_pack(channel[0] / 255.f, channel[1] / 255.f, channel[2] / 255.f, alpha);
        return true;
    }

    for (size_t i = 0; i < sizeof(svg_named_colors) / sizeof(svg_named_colors[0]); ++i)
    {
        if (sv_eq_ignorecase(sv, sv_from_cstr(svg_named_colors[i].name)))
        {
            *out = svg_color_pack((float)svg_named_colors[i].r / 255.f, (float)svg_named_colors[i].g / 255.f,
                                  (float)svg_named_colors[i].b / 255.f, 1.f);
            return true;
        }
    }

    return false;
}

//----------------------------------------------------------------------------------------------------------------------------
// numbers

// one SVG number at [i] : optional sign, digits and at most one dot, optional exponent
// (also accepts a leading dot) ; returns false when no number starts there
static bool svg_parse_number(String_View sv, size_t* i, float* out)
{
    size_t n = sv.count;
    size_t p = *i;
    if (p >= n)
        return false;

    const char* s = sv.data;
    size_t j = p;
    if (s[j] == '+' || s[j] == '-')
        ++j;

    bool has_digit = false;
    while (j < n && (isdigit((unsigned char)s[j]) || s[j] == '.'))
    {
        if (isdigit((unsigned char)s[j]))
            has_digit = true;
        ++j;
    }

    if (j < n && (s[j] == 'e' || s[j] == 'E'))
    {
        size_t k = j + 1;
        if (k < n && (s[k] == '+' || s[k] == '-'))
            ++k;
        bool exp_digit = false;
        while (k < n && isdigit((unsigned char)s[k]))
        {
            exp_digit = true;
            ++k;
        }
        if (exp_digit)
            j = k;
    }

    if (!has_digit)
        return false;

    double v = 0.0;
    double frac = 1.0;
    bool in_frac = false;
    bool neg = s[p] == '-';
    size_t k = p;
    if (s[k] == '+' || s[k] == '-')
        ++k;
    while (k < j && s[k] != 'e' && s[k] != 'E')
    {
        if (isdigit((unsigned char)s[k]))
        {
            if (in_frac)
            {
                v += (double)(s[k] - '0') * frac;
                frac *= .1;
            }
            else
                v = v * 10.0 + (double)(s[k] - '0');
        }
        else
        {
            in_frac = true;
            frac = .1;
        }
        ++k;
    }
    if (k < j)
    {
        int exp = 0;
        bool exp_neg = false;
        ++k;
        if (k < j && (s[k] == '+' || s[k] == '-'))
        {
            exp_neg = s[k] == '-';
            ++k;
        }
        while (k < j)
        {
            exp = exp * 10 + (s[k] - '0');
            ++k;
        }
        for (int e = 0; e < exp; ++e)
            v = exp_neg ? v / 10.0 : v * 10.0;
    }

    *i = j;
    *out = (float)(neg ? -v : v);
    return true;
}

// consecutive numbers separated by whitespace / commas / anything but a number start
static uint32_t svg_parse_numbers(String_View sv, float* out, uint32_t cap)
{
    uint32_t n = 0;
    size_t i = 0;
    while (i < sv.count && n < cap)
    {
        while (i < sv.count && (isspace((unsigned char)sv.data[i]) || sv.data[i] == ',' || sv.data[i] == ';' || sv.data[i] == '/'))
            ++i;
        if (i >= sv.count)
            break;
        size_t start = i;
        float v;
        if (!svg_parse_number(sv, &i, &v))
            break;
        (void) start;
        out[n++] = v;
    }
    return n;
}

// a single geometry value : a number, optionally followed by whitespace, a percent, or an alphabetic
// unit (px, pt, cm, ...) ; absolute units are taken as user units
static bool svg_parse_geom_value(String_View value, float* out)
{
    size_t i = 0;
    if (!svg_parse_number(value, &i, out))
        return false;
    size_t u = i;
    while (u < value.count && isalpha((unsigned char)value.data[u]))
        ++u;
    return i >= value.count || isspace((unsigned char)value.data[i]) || value.data[i] == '%' || u >= value.count;
}

//----------------------------------------------------------------------------------------------------------------------------
// entities

static uint32_t svg_entity_value(String_View sv)
{
    if (sv_eq(sv, SV("amp")))
        return '&';
    if (sv_eq(sv, SV("lt")))
        return '<';
    if (sv_eq(sv, SV("gt")))
        return '>';
    if (sv_eq(sv, SV("quot")))
        return '"';
    if (sv_eq(sv, SV("apos")))
        return '\'';
    if (sv.count > 0 && sv.data[0] == '#')
    {
        String_View n = sv_from_parts(sv.data + 1, sv.count - 1);
        if (n.count > 0 && (n.data[0] == 'x' || n.data[0] == 'X'))
        {
            uint32_t v = 0;
            n = sv_from_parts(n.data + 1, n.count - 1);
            for (size_t i = 0; i < n.count; ++i)
            {
                char c = n.data[i];
                uint32_t d;
                if (c >= '0' && c <= '9')
                    d = (uint32_t)(c - '0');
                else if (c >= 'a' && c <= 'f')
                    d = (uint32_t)(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F')
                    d = (uint32_t)(c - 'A' + 10);
                else
                    return 0;
                v = (v << 4) | d;
            }
            return v;
        }
        uint32_t v = 0;
        for (size_t i = 0; i < n.count && isdigit((unsigned char)n.data[i]); ++i)
            v = v * 10 + (uint32_t)(n.data[i] - '0');
        return v;
    }
    return 0;
}

// writes the entity-decoded content into [out] (capacity [cap], no null terminator), returns the length
static size_t svg_decode_entities(String_View in, char* out, size_t cap)
{
    size_t n = 0;
    size_t i = 0;
    while (i < in.count)
    {
        if (in.data[i] != '&')
        {
            if (n < cap)
                out[n] = in.data[i];
            ++n;
            ++i;
            continue;
        }
        size_t end = in.count;
        for (size_t k = i + 1; k < in.count; ++k)
        {
            if (in.data[k] == ';')
            {
                end = k;
                break;
            }
        }
        String_View ent = sv_from_parts(in.data + i + 1, end - (i + 1));
        uint32_t cp = svg_entity_value(ent);
        if (cp != 0 && n < cap)
        {
            out[n] = (char)cp;
            ++n;
        }
        i = (end < in.count) ? end + 1 : in.count;
    }
    return n;
}

//----------------------------------------------------------------------------------------------------------------------------
// xml tokenizer

typedef enum
{
    svg_tok_end,
    svg_tok_open,
    svg_tok_close,
    svg_tok_self,
    svg_tok_endtag,
    svg_tok_attr,
    svg_tok_value,
    svg_tok_text,
    svg_tok_skip
} svg_tok_type;

typedef struct
{
    svg_tok_type type;
    String_View data;
} svg_token;

static svg_token svg_xml_next(String_View* sv)
{
    svg_token t = {svg_tok_end, SV_NULL};
    String_View s = *sv;

    if (s.count == 0)
        return t;

    if (s.data[0] == '<')
    {
        if (s.count > 1 && s.data[1] == '?')
        {
            for (size_t i = 0; i + 1 < s.count; ++i)
            {
                if (s.data[i] == '?' && s.data[i + 1] == '>')
                {
                    *sv = sv_from_parts(s.data + i + 2, s.count - i - 2);
                    t.type = svg_tok_skip;
                    return t;
                }
            }
            *sv = sv_from_parts(s.data + s.count, 0);
            t.type = svg_tok_skip;
            return t;
        }

        if (s.count > 3 && s.data[1] == '!' && s.data[2] == '-' && s.data[3] == '-')
        {
            for (size_t i = 0; i + 2 < s.count; ++i)
            {
                if (s.data[i] == '-' && s.data[i + 1] == '-' && s.data[i + 2] == '>')
                {
                    *sv = sv_from_parts(s.data + i + 3, s.count - i - 3);
                    t.type = svg_tok_skip;
                    return t;
                }
            }
            *sv = sv_from_parts(s.data + s.count, 0);
            t.type = svg_tok_skip;
            return t;
        }

        if (s.count > 1 && s.data[1] == '!')
        {
            size_t i = 0;
            while (i < s.count && s.data[i] != '>')
                ++i;
            *sv = sv_from_parts(s.data + i + 1, s.count - i - 1);
            t.type = svg_tok_skip;
            return t;
        }

        if (s.count > 1 && s.data[1] == '/')
        {
            size_t i = 2;
            while (i < s.count && !isspace((unsigned char)s.data[i]) && s.data[i] != '>')
                ++i;
            t.type = svg_tok_endtag;
            t.data = sv_from_parts(s.data + 2, i - 2);
            while (i < s.count && s.data[i] != '>')
                ++i;
            *sv = sv_from_parts(s.data + i + 1, s.count - i - 1);
            return t;
        }

        {
            size_t i = 1;
            while (i < s.count && !isspace((unsigned char)s.data[i]) && s.data[i] != '>' && s.data[i] != '/')
                ++i;
            t.type = svg_tok_open;
            t.data = sv_from_parts(s.data + 1, i - 1);
            *sv = sv_from_parts(s.data + i, s.count - i);
            return t;
        }
    }

    size_t i = 0;
    while (i < s.count && s.data[i] != '<')
        ++i;
    t.type = svg_tok_text;
    t.data = sv_from_parts(s.data, i);
    *sv = sv_from_parts(s.data + i, s.count - i);
    return t;
}

// attr or the tag end ; [pending] is the attribute name waiting for its value
static svg_token svg_xml_attr_or_end(String_View* sv)
{
    svg_token t = {svg_tok_end, SV_NULL};
    String_View s = *sv;

    s = sv_trim_left(s);
    if (s.count == 0)
    {
        *sv = s;
        return t;
    }

    if (s.data[0] == '/')
    {
        size_t i = 1;
        while (i < s.count && s.data[i] != '>')
            ++i;
        *sv = sv_from_parts(s.data + i + 1, s.count - i - 1);
        t.type = svg_tok_self;
        return t;
    }

    if (s.data[0] == '>')
    {
        *sv = sv_from_parts(s.data + 1, s.count - 1);
        t.type = svg_tok_close;
        return t;
    }

    size_t i = 0;
    while (i < s.count && !isspace((unsigned char)s.data[i]) && s.data[i] != '=' && s.data[i] != '/' && s.data[i] != '>')
        ++i;
    t.type = svg_tok_attr;
    t.data = sv_from_parts(s.data, i);
    *sv = sv_from_parts(s.data + i, s.count - i);
    return t;
}

// the value following an attribute name (optional '=', quoted)
static svg_token svg_xml_value(String_View* sv)
{
    svg_token t = {svg_tok_end, SV_NULL};
    String_View s = *sv;

    s = sv_trim_left(s);
    if (s.count > 0 && s.data[0] == '=')
        s = sv_trim_left(sv_from_parts(s.data + 1, s.count - 1));
    if (s.count == 0)
    {
        *sv = s;
        return t;
    }

    char q = s.data[0];
    if (q != '"' && q != '\'')
    {
        size_t i = 0;
        while (i < s.count && !isspace((unsigned char)s.data[i]) && s.data[i] != '>' && s.data[i] != '/')
            ++i;
        t.type = svg_tok_value;
        t.data = sv_from_parts(s.data, i);
        *sv = sv_from_parts(s.data + i, s.count - i);
        return t;
    }

    size_t i = 1;
    while (i < s.count && s.data[i] != q)
        ++i;
    t.type = svg_tok_value;
    t.data = sv_from_parts(s.data + 1, i - 1);
    *sv = sv_from_parts(s.data + i + 1, s.count - i - 1);
    return t;
}

//----------------------------------------------------------------------------------------------------------------------------
// style declarations

static void svg_style_apply_mask(svg_style* into, const svg_style* from, uint32_t mask)
{
    if (mask & svg_set_color) into->color = from->color;
    if (mask & svg_set_fill) { into->fill = from->fill; into->fill_none = from->fill_none; into->fill_current = from->fill_current; }
    if (mask & svg_set_stroke) { into->stroke = from->stroke; into->stroke_none = from->stroke_none; into->stroke_current = from->stroke_current; }
    if (mask & svg_set_opacity) into->opacity = from->opacity;
    if (mask & svg_set_fill_opacity) into->fill_opacity = from->fill_opacity;
    if (mask & svg_set_stroke_opacity) into->stroke_opacity = from->stroke_opacity;
    if (mask & svg_set_stroke_width) into->stroke_width = from->stroke_width;
    if (mask & svg_set_linecap) into->linecap = from->linecap;
    if (mask & svg_set_linejoin) into->linejoin = from->linejoin;
    if (mask & svg_set_miterlimit) into->miterlimit = from->miterlimit;
    if (mask & svg_set_dash) { into->dash[0] = from->dash[0]; into->dash[1] = from->dash[1]; into->num_dash = from->num_dash; into->dash_set = from->dash_set; }
    if (mask & svg_set_dash_offset) into->dash_offset = from->dash_offset;
    if (mask & svg_set_font) into->font = from->font;
    if (mask & svg_set_font_size) into->font_size = from->font_size;
    if (mask & svg_set_anchor) into->anchor = from->anchor;
    if (mask & svg_set_display) into->display = from->display;
    if (mask & svg_set_visibility) into->visibility = from->visibility;
}

static void svg_style_set_paint(uint32_t* flags, String_View value, uint32_t* color, bool* none, bool* current)
{
    if (sv_starts_with(sv_trim(value), SV("url(")))
    {
        *flags |= svg_flag_paint_unsupported;
        *color = 0xff000000;
        *none = false;
        *current = false;
        return;
    }
    if (!svg_parse_color(value, color, none, current))
    {
        *color = 0xff000000;
        *none = false;
        *current = false;
    }
}

static void svg_style_set_prop(uint32_t* flags, svg_style* s, uint32_t* mask, String_View name, String_View value)
{
    value = sv_trim(value);
    size_t i = 0;
    float f;

    if (sv_eq(name, SV("fill")))
    {
        svg_style_set_paint(flags, value, &s->fill, &s->fill_none, &s->fill_current);
        *mask |= svg_set_fill;
    }
    else if (sv_eq(name, SV("stroke")))
    {
        svg_style_set_paint(flags, value, &s->stroke, &s->stroke_none, &s->stroke_current);
        *mask |= svg_set_stroke;
    }
    else if (sv_eq(name, SV("color")))
    {
        bool none, current;
        if (svg_parse_color(value, &s->color, &none, &current))
            *mask |= svg_set_color;
    }
    else if (sv_eq(name, SV("opacity")))
    {
        if (svg_parse_number(value, &i, &f) && i == value.count)
        {
            if (f < 0.f) f = 0.f;
            if (f > 1.f) f = 1.f;
            s->opacity = f;
            *mask |= svg_set_opacity;
        }
    }
    else if (sv_eq(name, SV("fill-opacity")) || sv_eq(name, SV("stroke-opacity")))
    {
        if (svg_parse_number(value, &i, &f) && i == value.count)
        {
            if (f < 0.f) f = 0.f;
            if (f > 1.f) f = 1.f;
            if (name.data[0] == 'f')
                s->fill_opacity = f;
            else
                s->stroke_opacity = f;
            *mask |= sv_eq(name, SV("fill-opacity")) ? svg_set_fill_opacity : svg_set_stroke_opacity;
        }
    }
    else if (sv_eq(name, SV("stroke-width")))
    {
        if (svg_parse_number(value, &i, &f))
        {
            if (f < 0.f) f = 0.f;
            s->stroke_width = f;
            *mask |= svg_set_stroke_width;
        }
    }
    else if (sv_eq(name, SV("stroke-linecap")))
    {
        if (sv_eq(value, SV("round"))) s->linecap = 1;
        else if (sv_eq(value, SV("square"))) s->linecap = 2;
        else s->linecap = 0;
        *mask |= svg_set_linecap;
    }
    else if (sv_eq(name, SV("stroke-linejoin")))
    {
        if (sv_eq(value, SV("round"))) s->linejoin = 1;
        else if (sv_eq(value, SV("bevel"))) s->linejoin = 2;
        else s->linejoin = 0;
        *mask |= svg_set_linejoin;
    }
    else if (sv_eq(name, SV("stroke-miterlimit")))
    {
        if (svg_parse_number(value, &i, &f))
        {
            s->miterlimit = f;
            *mask |= svg_set_miterlimit;
        }
    }
    else if (sv_eq(name, SV("stroke-dasharray")))
    {
        if (sv_eq(value, SV("none")))
        {
            s->dash_set = 0;
            s->num_dash = 0;
        }
        else
        {
            float v[8];
            uint32_t n = svg_parse_numbers(value, v, 8);
            if (n > 0)
            {
                s->num_dash = (uint8_t)(n > 2 ? 2 : n);
                s->dash_set = 1;
                s->dash[0] = v[0];
                if (n > 1)
                    s->dash[1] = v[1];
                if (n > 2)
                    *flags |= svg_flag_stroke_unsupported;
            }
        }
        *mask |= svg_set_dash;
    }
    else if (sv_eq(name, SV("stroke-dashoffset")))
    {
        if (svg_parse_number(value, &i, &f))
        {
            s->dash_offset = f;
            *mask |= svg_set_dash_offset;
        }
    }
    else if (sv_eq(name, SV("font-family")))
    {
        for (size_t k = 0; k + 4 < value.count; ++k)
            if (sv_eq_ignorecase(sv_from_parts(value.data + k, 4), SV("mono")))
            {
                s->font = od_font_mono;
                *mask |= svg_set_font;
                return;
            }
    }
    else if (sv_eq(name, SV("font-weight")))
    {
        bool bold = sv_eq(value, SV("bold")) || sv_eq(value, SV("bolder")) || sv_eq(value, SV("800")) || sv_eq(value, SV("900"));
        if (!bold && svg_parse_number(value, &i, &f))
            bold = f >= 600.f;
        if (bold && s->font != od_font_mono)
        {
            s->font = od_font_bold;
            *mask |= svg_set_font;
        }
    }
    else if (sv_eq(name, SV("font-style")))
    {
        if ((sv_eq(value, SV("italic")) || sv_eq(value, SV("oblique"))) && s->font != od_font_mono)
        {
            s->font = od_font_italic;
            *mask |= svg_set_font;
        }
    }
    else if (sv_eq(name, SV("font-size")))
    {
        if (svg_parse_number(value, &i, &f))
        {
            s->font_size = f;
            *mask |= svg_set_font_size;
        }
    }
    else if (sv_eq(name, SV("text-anchor")))
    {
        if (sv_eq(value, SV("middle"))) s->anchor = 1;
        else if (sv_eq(value, SV("end"))) s->anchor = 2;
        else s->anchor = 0;
        *mask |= svg_set_anchor;
    }
    else if (sv_eq(name, SV("display")))
    {
        s->display = sv_eq(value, SV("none")) ? 1 : 0;
        *mask |= svg_set_display;
    }
    else if (sv_eq(name, SV("visibility")))
    {
        s->visibility = sv_eq(value, SV("hidden")) ? 1 : 0;
        *mask |= svg_set_visibility;
    }
}

// one "prop : value" declaration
static void svg_style_set_decl(uint32_t* flags, svg_style* s, uint32_t* mask, String_View decl)
{
    decl = sv_trim(decl);
    size_t colon = 0;
    for (size_t k = 0; k < decl.count; ++k)
    {
        if (decl.data[k] == ':')
        {
            colon = k;
            break;
        }
    }
    if (colon == 0)
        return;
    svg_style_set_prop(flags, s, mask, sv_from_parts(decl.data, colon), sv_from_parts(decl.data + colon + 1, decl.count - colon - 1));
}

// a full style="..." attribute
static void svg_style_set_attr(uint32_t* flags, svg_style* s, uint32_t* mask, String_View value)
{
    String_View rest = value;
    while (rest.count > 0)
    {
        size_t semi = 0;
        bool found = false;
        for (size_t k = 0; k < rest.count; ++k)
        {
            if (rest.data[k] == ';')
            {
                semi = k;
                found = true;
                break;
            }
        }
        if (found)
        {
            svg_style_set_decl(flags, s, mask, sv_from_parts(rest.data, semi));
            rest = sv_from_parts(rest.data + semi + 1, rest.count - semi - 1);
        }
        else
        {
            svg_style_set_decl(flags, s, mask, rest);
            break;
        }
    }
}

//----------------------------------------------------------------------------------------------------------------------------
// transforms

static bool svg_transform_func(uint32_t* flags, String_View sv, svg_xform* acc)
{
    (void) flags;
    sv = sv_trim_left(sv);
    size_t name_end = 0;
    while (name_end < sv.count && sv.data[name_end] != '(')
        ++name_end;
    if (name_end >= sv.count)
        return false;
    String_View name = sv_from_parts(sv.data, name_end);
    String_View rest = sv_from_parts(sv.data + name_end + 1, sv.count - name_end - 1);

    if (!sv_ends_with(rest, SV(")")))
        return false;
    rest = sv_from_parts(rest.data, rest.count - 1);

    float v[6] = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
    uint32_t n = svg_parse_numbers(rest, v, 6);

    svg_xform m = svg_xform_identity();
    if (sv_eq(name, SV("matrix")))
    {
        if (n != 6)
            return false;
        m.a = v[0]; m.b = v[1]; m.c = v[2]; m.d = v[3]; m.e = v[4]; m.f = v[5];
    }
    else if (sv_eq(name, SV("translate")))
    {
        if (n < 1)
            return false;
        m.e = v[0];
        m.f = (n > 1) ? v[1] : 0.f;
    }
    else if (sv_eq(name, SV("scale")))
    {
        if (n < 1)
            return false;
        m.a = v[0];
        m.d = (n > 1) ? v[1] : v[0];
    }
    else if (sv_eq(name, SV("rotate")))
    {
        if (n < 1)
            return false;
        float rad = (float)(v[0] * M_PI / 180.0);
        float cs = cosf(rad), sn = sinf(rad);
        m.a = cs; m.b = sn; m.c = -sn; m.d = cs;
        if (n > 2)
        {
            // rotate about (cx, cy)
            m.e = v[1] - v[1] * cs + v[2] * sn;
            m.f = v[2] - v[1] * sn - v[2] * cs;
        }
    }
    else if (sv_eq(name, SV("skewX")))
    {
        if (n < 1)
            return false;
        m.c = tanf((float)(v[0] * M_PI / 180.0));
    }
    else if (sv_eq(name, SV("skewY")))
    {
        if (n < 1)
            return false;
        m.b = tanf((float)(v[0] * M_PI / 180.0));
    }
    else
        return false;

    *acc = svg_xform_mul(acc, &m);
    return true;
}

static bool svg_parse_transforms(uint32_t* flags, String_View sv, svg_xform* acc)
{
    while (sv.count > 0)
    {
        sv = sv_trim_left(sv);
        if (sv.count == 0)
            break;
        size_t close = 0;
        bool found = false;
        for (size_t k = 0; k < sv.count; ++k)
        {
            if (sv.data[k] == ')')
            {
                close = k;
                found = true;
                break;
            }
        }
        if (!found)
            return false;
        if (!svg_transform_func(flags, sv_from_parts(sv.data, close + 1), acc))
            return false;
        sv = sv_from_parts(sv.data + close + 1, sv.count - close - 1);
    }
    return true;
}

//----------------------------------------------------------------------------------------------------------------------------
// builder

typedef struct svg_bld_elem
{
    uint8_t type;
    uint16_t num_params;
    uint32_t num_points;
    uint32_t num_path;
    uint32_t text_len;
} svg_bld_elem;

typedef struct svg_bld
{
    svg_mem mem;
    bool count;
    bool error;
    uint32_t flags;

    uint32_t num_elements;
    uint32_t num_params;
    uint32_t num_points;
    uint32_t num_path;
    uint32_t num_text;
    uint32_t num_strings;
    uint32_t num_ids;
    uint32_t num_rules;
    uint32_t num_rule_strs;
    uint32_t rule_strs_off;
    svg_bld_elem* tmp;
    size_t tmp_cap;

    svg_document* doc;
    uint32_t params_off;
    uint32_t points_off;
    uint32_t path_off;
    uint32_t text_off;
    uint32_t strings_off;

    uint32_t stack[64];
    uint32_t stack_last[64];
    uint32_t depth;

    float p[11];
    uint32_t nparams;
    uint32_t param_mask;
    svg_xform xform;
    svg_style pres;
    uint32_t pres_mask;
    svg_style inl;
    uint32_t inl_mask;
    uint32_t text_mask;
    uint32_t text_depth;
    char* text_scratch;
    size_t text_len;
    size_t text_cap;
    bool in_text;
    bool in_style;
    char* style_text;
    size_t style_len;
    size_t style_cap;
    size_t style_start;
    char* vbuf;
    size_t vbuf_cap;
    float root_w;
    float root_h;
    bool root_size;
} svg_bld;

static void svg_parse_rules(svg_bld* b, String_View text);

static void* bld_grow(svg_bld* b, void** ptr, size_t* cap, size_t elem_size, size_t need)
{
    if (*cap >= need)
        return *ptr;
    size_t nc = *cap ? *cap : 16;
    while (nc < need)
        nc *= 2;
    void* np = svg_mem_alloc(&b->mem, nc * elem_size, sizeof(void*));
    if (np == NULL)
    {
        b->error = true;
        return NULL;
    }
    if (*ptr != NULL)
        memcpy(np, *ptr, *cap * elem_size);
    *ptr = np;
    *cap = nc;
    return np;
}

static uint32_t svg_el_type_from_name(String_View name)
{
    if (sv_eq(name, SV("g"))) return svg_el_g;
    if (sv_eq(name, SV("defs"))) return svg_el_defs;
    if (sv_eq(name, SV("symbol"))) return svg_el_symbol;
    if (sv_eq(name, SV("use"))) return svg_el_use;
    if (sv_eq(name, SV("svg"))) return svg_el_svg;
    if (sv_eq(name, SV("rect"))) return svg_el_rect;
    if (sv_eq(name, SV("circle"))) return svg_el_circle;
    if (sv_eq(name, SV("ellipse"))) return svg_el_ellipse;
    if (sv_eq(name, SV("line"))) return svg_el_line;
    if (sv_eq(name, SV("polygon"))) return svg_el_polygon;
    if (sv_eq(name, SV("polyline"))) return svg_el_polyline;
    if (sv_eq(name, SV("path"))) return svg_el_path;
    if (sv_eq(name, SV("text"))) return svg_el_text;
    if (sv_eq(name, SV("tspan"))) return svg_el_tspan;
    if (sv_eq(name, SV("clipPath"))) return svg_el_clip_path;
    if (sv_eq(name, SV("image"))) return svg_el_image;
    return svg_el_unknown;
}

static bool svg_el_drops_content(String_View name)
{
    return sv_eq(name, SV("foreignObject")) || sv_eq(name, SV("switch")) || sv_eq(name, SV("script")) ||
           sv_eq(name, SV("animate")) || sv_eq(name, SV("animateColor")) || sv_eq(name, SV("animateTransform")) ||
           sv_eq(name, SV("set"));
}

static uint32_t bld_bump_tmp(svg_bld* b, uint32_t type)
{
    uint32_t idx = b->num_elements++;
    if (b->tmp == NULL || b->num_elements > b->tmp_cap)
    {
        bld_grow(b, (void**)&b->tmp, &b->tmp_cap, sizeof(svg_bld_elem), b->num_elements + 8);
        if (b->error)
            return SVG_INVALID;
    }
    b->tmp[idx].type = (uint8_t)type;
    b->tmp[idx].num_params = 0;
    b->tmp[idx].num_points = 0;
    b->tmp[idx].num_path = 0;
    b->tmp[idx].text_len = 0;
    return idx;
}

static uint32_t bld_strings(svg_bld* b, String_View s)
{
    if (b->count)
    {
        b->num_strings += (uint32_t)s.count;
        return 0;
    }
    uint32_t off = b->strings_off;
    if (s.count > 0)
    {
        memcpy(b->doc->strings + b->strings_off, s.data, s.count);
        b->strings_off += (uint32_t)s.count;
    }
    return off;
}

static uint32_t bld_begin(svg_bld* b, String_View name)
{
    uint32_t type = svg_el_type_from_name(name);
    uint32_t idx = bld_bump_tmp(b, type);
    if (idx == SVG_INVALID)
        return SVG_INVALID;
    if (svg_el_drops_content(name))
        b->flags |= svg_flag_content_dropped;

    uint32_t name_off = bld_strings(b, name);
    if (!b->count)
    {
        svg_element* e = &b->doc->elements[idx];
        memset(e, 0, sizeof(*e));
        e->type = type;
        e->first_child = SVG_INVALID;
        e->next_sibling = SVG_INVALID;
        e->parent = SVG_INVALID;
        e->points = SVG_INVALID;
        e->path = SVG_INVALID;
        e->text = SVG_INVALID;
        e->id = SVG_INVALID;
        e->transform = idx * 6;
        e->name_off = name_off;
        e->name_len = (uint32_t)name.count;

        if (b->depth > 0)
        {
            uint32_t parent = b->stack[b->depth - 1];
            e->parent = parent;
            if (b->doc->elements[parent].first_child == SVG_INVALID)
                b->doc->elements[parent].first_child = idx;
            else
                b->doc->elements[b->stack_last[b->depth - 1]].next_sibling = idx;
            b->stack_last[b->depth - 1] = idx;
        }
    }

    if (b->depth >= 64)
    {
        b->error = true;
        return idx;
    }
    b->stack[b->depth] = idx;
    b->stack_last[b->depth] = idx;
    ++b->depth;

    // a child starts inside a text element : the parent's own text (everything in the scratch so
    // far) is complete; the scratch is then reused for the child's content
    if (b->depth > 1 && b->in_text && b->text_len > 0)
    {
        uint32_t parent = b->stack[b->depth - 2];
        if ((b->tmp[parent].type == svg_el_text || b->tmp[parent].type == svg_el_tspan) && b->tmp[parent].text_len == 0)
        {
            b->tmp[parent].text_len = (uint32_t)b->text_len;
            if (!b->count)
            {
                svg_element* pe = &b->doc->elements[parent];
                pe->text = b->text_off;
                memcpy(b->doc->text + b->text_off, b->text_scratch, b->text_len);
                b->text_off += (uint32_t)b->text_len;
                pe->text_len = (uint32_t)b->text_len;
            }
        }
        b->text_len = 0;
    }

    b->nparams = 0;
    b->param_mask = 0;
    memset(b->p, 0, sizeof(b->p));
    b->xform = svg_xform_identity();
    b->pres = svg_style_defaults();
    b->pres_mask = 0;
    b->inl = svg_style_defaults();
    b->inl_mask = 0;
    b->text_mask = 0;
    b->text_len = 0;
    if (type == svg_el_text || type == svg_el_tspan)
        ++b->text_depth;
    b->in_text = b->text_depth > 0;
    b->in_style = (type == svg_el_unknown && sv_eq(name, SV("style")));
    if (b->in_style)
        b->style_start = b->style_len;
    if (b->in_text)
    {
        bld_grow(b, (void**)&b->text_scratch, &b->text_cap, 1, 64);
        if (b->error)
            return idx;
    }
    return idx;
}

// The element's own state (params, transform, styles) is complete once its start tag is done, but the
// live builder state is then clobbered by its children, so a parent's end tag can no longer recover
// it : store it here, right after the start tag.
static void bld_snapshot(svg_bld* b, uint32_t idx)
{
    uint32_t type = b->count ? b->tmp[idx].type : b->doc->elements[idx].type;

    if (type == svg_el_text || type == svg_el_tspan)
    {
        uint32_t mask_slot = (type == svg_el_text) ? 2 : 4;
        b->p[mask_slot] = (float)b->text_mask;
        if (b->nparams < mask_slot + 1)
            b->nparams = mask_slot + 1;
    }

    if (type == svg_el_svg && !b->root_size)
    {
        float w = (b->param_mask & (1u << 2)) ? b->p[2] : 0.f;
        float h = (b->param_mask & (1u << 3)) ? b->p[3] : 0.f;
        if (w <= 0.f && (b->param_mask & (1u << 6)))
            w = b->p[6];
        if (h <= 0.f && (b->param_mask & (1u << 7)))
            h = b->p[7];
        b->root_w = (w > 0.f) ? w : 100.f;
        b->root_h = (h > 0.f) ? h : 100.f;
        b->root_size = true;
    }

    if (b->count)
    {
        b->tmp[idx].num_params = b->nparams;
        return;
    }

    svg_element* e = &b->doc->elements[idx];
    if (b->nparams > 0)
    {
        e->params = b->params_off;
        memcpy(b->doc->params + b->params_off, b->p, b->nparams * sizeof(float));
        b->params_off += b->nparams;
        e->num_params = b->nparams;
        e->param_mask = b->param_mask;
    }
    float* m = b->doc->transforms + e->transform;
    m[0] = b->xform.a;
    m[1] = b->xform.b;
    m[2] = b->xform.c;
    m[3] = b->xform.d;
    m[4] = b->xform.e;
    m[5] = b->xform.f;
    b->doc->pres[idx] = b->pres;
    b->doc->pres_mask[idx] = b->pres_mask;
    b->doc->inl[idx] = b->inl;
    b->doc->inl_mask[idx] = b->inl_mask;
}

static void bld_points(svg_bld* b, uint32_t idx, String_View value)
{
    size_t i = 0;
    uint32_t n = 0;
    float x, y;

    while (i < value.count)
    {
        while (i < value.count && (isspace((unsigned char)value.data[i]) || value.data[i] == ',' || value.data[i] == ';' || value.data[i] == '/'))
            ++i;
        if (i >= value.count)
            break;
        if (!svg_parse_number(value, &i, &x))
            return;
        while (i < value.count && (isspace((unsigned char)value.data[i]) || value.data[i] == ',' || value.data[i] == ';' || value.data[i] == '/'))
            ++i;
        if (i >= value.count || !svg_parse_number(value, &i, &y))
            return;
        if (!b->count)
        {
            svg_element* e = &b->doc->elements[idx];
            if (e->points == SVG_INVALID)
                e->points = b->points_off;
            b->doc->points[b->points_off++] = x;
            b->doc->points[b->points_off++] = y;
        }
        ++n;
    }

    if (b->count)
    {
        b->num_points += n * 2;
        b->tmp[idx].num_points += n * 2;
    }
    else
    {
        svg_element* e = &b->doc->elements[idx];
        e->num_points += n * 2;
    }
}

static void bld_path_cmd(svg_bld* b, uint32_t idx, uint8_t op, const float* v, uint32_t n)
{
    if (b->count)
    {
        b->num_path++;
        b->tmp[idx].num_path++;
        return;
    }
    svg_element* e = &b->doc->elements[idx];
    if (e->path == SVG_INVALID)
        e->path = b->path_off;
    e->num_path++;
    svg_path_cmd* c = &b->doc->paths[b->path_off++];
    c->op = op;
    c->count = (uint8_t)n;
    for (uint32_t k = 0; k < n; ++k)
        c->v[k] = v[k];
}

static void bld_path_d(svg_bld* b, uint32_t idx, String_View d)
{
    float cur_x = 0.f, cur_y = 0.f, start_x = 0.f, start_y = 0.f;
    float last_cx = 0.f, last_cy = 0.f;
    bool have_last_cubic = false, have_last_quad = false;
    size_t i = 0;

    while (i < d.count)
    {
        while (i < d.count && (isspace((unsigned char)d.data[i]) || d.data[i] == ','))
            ++i;
        if (i >= d.count)
            break;

        char cmd = d.data[i++];
        bool rel = cmd >= 'a' && cmd <= 'z';
        char op = (char)(cmd & ~0x20);
        float v[7];
        uint32_t nv = 0;

        switch (op)
        {
        case 'M':
        case 'L':
            while (nv < 2 && i < d.count)
            {
                while (i < d.count && (isspace((unsigned char)d.data[i]) || d.data[i] == ','))
                    ++i;
                if (i >= d.count)
                    break;
                if (!svg_parse_number(d, &i, &v[nv]))
                    return;
                ++nv;
            }
            if (nv < 2)
                return;
            if (rel)
            {
                v[0] += cur_x;
                v[1] += cur_y;
            }
            cur_x = v[0];
            cur_y = v[1];
            if (op == 'M')
            {
                start_x = cur_x;
                start_y = cur_y;
                bld_path_cmd(b, idx, svg_path_move, v, 2);
            }
            else
                bld_path_cmd(b, idx, svg_path_line, v, 2);
            // subsequent pairs after M are implicit lines
            if (op == 'M')
            {
                while (i < d.count)
                {
                    while (i < d.count && (isspace((unsigned char)d.data[i]) || d.data[i] == ','))
                        ++i;
                    if (i >= d.count || (d.data[i] >= 'a' && d.data[i] <= 'z') || (d.data[i] >= 'A' && d.data[i] <= 'Z'))
                        break;
                    float lv[2];
                    uint32_t lvn = 0;
                    while (lvn < 2 && i < d.count)
                    {
                        if (!svg_parse_number(d, &i, &lv[lvn]))
                            return;
                        ++lvn;
                    }
                    if (lvn < 2)
                        return;
                    if (rel)
                    {
                        lv[0] += cur_x;
                        lv[1] += cur_y;
                    }
                    cur_x = lv[0];
                    cur_y = lv[1];
                    bld_path_cmd(b, idx, svg_path_line, lv, 2);
                }
            }
            have_last_cubic = false;
            have_last_quad = false;
            break;
        case 'C':
            while (nv < 6 && i < d.count)
            {
                while (i < d.count && (isspace((unsigned char)d.data[i]) || d.data[i] == ','))
                    ++i;
                if (i >= d.count)
                    break;
                if (!svg_parse_number(d, &i, &v[nv]))
                    return;
                if (rel)
                {
                    v[nv] += (nv % 2 == 0) ? cur_x : cur_y;
                }
                ++nv;
            }
            if (nv < 6)
                return;
            last_cx = v[4];
            last_cy = v[5];
            cur_x = v[4];
            cur_y = v[5];
            have_last_cubic = true;
            have_last_quad = false;
            bld_path_cmd(b, idx, svg_path_cubic, v, 6);
            break;
        case 'S':
        {
            while (nv < 4 && i < d.count)
            {
                while (i < d.count && (isspace((unsigned char)d.data[i]) || d.data[i] == ','))
                    ++i;
                if (i >= d.count)
                    break;
                if (!svg_parse_number(d, &i, &v[nv]))
                    return;
                if (rel)
                    v[nv] += (nv % 2 == 0) ? cur_x : cur_y;
                ++nv;
            }
            if (nv < 4)
                return;
            float f[6];
            if (have_last_cubic)
            {
                f[0] = 2.f * cur_x - last_cx;
                f[1] = 2.f * cur_y - last_cy;
            }
            else
            {
                f[0] = cur_x;
                f[1] = cur_y;
            }
            f[2] = v[0];
            f[3] = v[1];
            f[4] = v[2];
            f[5] = v[3];
            last_cx = v[2];
            last_cy = v[3];
            cur_x = v[2];
            cur_y = v[3];
            have_last_cubic = true;
            have_last_quad = false;
            bld_path_cmd(b, idx, svg_path_cubic, f, 6);
            break;
        }
        case 'Q':
            while (nv < 4 && i < d.count)
            {
                while (i < d.count && (isspace((unsigned char)d.data[i]) || d.data[i] == ','))
                    ++i;
                if (i >= d.count)
                    break;
                if (!svg_parse_number(d, &i, &v[nv]))
                    return;
                if (rel)
                    v[nv] += (nv % 2 == 0) ? cur_x : cur_y;
                ++nv;
            }
            if (nv < 4)
                return;
            last_cx = v[2];
            last_cy = v[3];
            cur_x = v[2];
            cur_y = v[3];
            have_last_quad = true;
            have_last_cubic = false;
            bld_path_cmd(b, idx, svg_path_quadratic, v, 4);
            break;
        case 'T':
        {
            while (nv < 2 && i < d.count)
            {
                while (i < d.count && (isspace((unsigned char)d.data[i]) || d.data[i] == ','))
                    ++i;
                if (i >= d.count)
                    break;
                if (!svg_parse_number(d, &i, &v[nv]))
                    return;
                if (rel)
                    v[nv] += (nv % 2 == 0) ? cur_x : cur_y;
                ++nv;
            }
            if (nv < 2)
                return;
            float f[4];
            if (have_last_quad)
            {
                f[0] = 2.f * cur_x - last_cx;
                f[1] = 2.f * cur_y - last_cy;
            }
            else
            {
                f[0] = cur_x;
                f[1] = cur_y;
            }
            f[2] = v[0];
            f[3] = v[1];
            last_cx = v[0];
            last_cy = v[1];
            cur_x = v[0];
            cur_y = v[1];
            have_last_quad = true;
            have_last_cubic = false;
            bld_path_cmd(b, idx, svg_path_quadratic, f, 4);
            break;
        }
        case 'H':
            while (i < d.count && (isspace((unsigned char)d.data[i]) || d.data[i] == ','))
                ++i;
            if (!svg_parse_number(d, &i, &v[0]))
                return;
            cur_x = (rel) ? cur_x + v[0] : v[0];
            bld_path_cmd(b, idx, svg_path_hline, &cur_x, 1);
            have_last_cubic = false;
            have_last_quad = false;
            break;
        case 'V':
            while (i < d.count && (isspace((unsigned char)d.data[i]) || d.data[i] == ','))
                ++i;
            if (!svg_parse_number(d, &i, &v[0]))
                return;
            cur_y = (rel) ? cur_y + v[0] : v[0];
            bld_path_cmd(b, idx, svg_path_vline, &cur_y, 1);
            have_last_cubic = false;
            have_last_quad = false;
            break;
        case 'A':
            while (nv < 7 && i < d.count)
            {
                while (i < d.count && (isspace((unsigned char)d.data[i]) || d.data[i] == ','))
                    ++i;
                if (i >= d.count)
                    break;
                if (!svg_parse_number(d, &i, &v[nv]))
                    return;
                ++nv;
            }
            if (nv < 7)
                return;
            if (rel)
            {
                v[6] += cur_x;
                v[5] += cur_y;
            }
            cur_x = v[6];
            cur_y = v[5];
            have_last_cubic = false;
            have_last_quad = false;
            bld_path_cmd(b, idx, svg_path_arc, v, 7);
            break;
        case 'Z':
            cur_x = start_x;
            cur_y = start_y;
            have_last_cubic = false;
            have_last_quad = false;
            bld_path_cmd(b, idx, svg_path_close, NULL, 0);
            break;
        default:
            return;
        }
    }
}

static void bld_end(svg_bld* b, String_View name)
{
    if (b->depth == 0)
        return;
    uint32_t idx = b->stack[b->depth - 1];
    --b->depth;
    uint32_t type = b->tmp[idx].type;

    if (b->in_style && b->style_len > b->style_start)
        svg_parse_rules(b, sv_from_parts(b->style_text + b->style_start, b->style_len - b->style_start));
    b->in_style = false;

    // store what is left in the scratch (an element whose text was already stored when a child
    // started keeps only the earlier fragment)
    if ((type == svg_el_text || type == svg_el_tspan) && b->text_len > 0 && b->tmp[idx].text_len == 0)
    {
        if (b->count)
            b->tmp[idx].text_len = (uint32_t)b->text_len;
        else
        {
            svg_element* e = &b->doc->elements[idx];
            e->text = b->text_off;
            memcpy(b->doc->text + b->text_off, b->text_scratch, b->text_len);
            b->text_off += (uint32_t)b->text_len;
            e->text_len = (uint32_t)b->text_len;
        }
    }

    if (type == svg_el_text || type == svg_el_tspan)
    {
        --b->text_depth;
        b->in_text = b->text_depth > 0;
        b->text_len = 0;
    }

    if (b->count)
    {
        b->num_params += b->tmp[idx].num_params;
        b->num_text += b->tmp[idx].text_len;
        return;
    }

    (void) name;
}

static void bld_text(svg_bld* b, String_View raw)
{
    if (b->in_style)
    {
        if (b->style_len + raw.count + 1 > b->style_cap)
        {
            bld_grow(b, (void**)&b->style_text, &b->style_cap, 1, b->style_len + raw.count + 1);
            if (b->error)
                return;
        }
        if (raw.count > 0)
        {
            memcpy(b->style_text + b->style_len, raw.data, raw.count);
            b->style_len += raw.count;
        }
        b->style_text[b->style_len++] = '\n';
        return;
    }
    if (!b->in_text)
        return;

    size_t i = 0;
    while (i < raw.count)
    {
        uint32_t cp = 0;
        if (raw.data[i] == '&')
        {
            size_t end = raw.count;
            for (size_t k = i + 1; k < raw.count; ++k)
            {
                if (raw.data[k] == ';')
                {
                    end = k;
                    break;
                }
            }
            cp = svg_entity_value(sv_from_parts(raw.data + i + 1, end - (i + 1)));
            if (cp == 0)
            {
                cp = (uint32_t)'&';
            }
            else
                i = (end < raw.count) ? end + 1 : raw.count;
        }
        else
        {
            cp = (uint32_t)(unsigned char)raw.data[i];
            ++i;
        }
        char c = (char)cp;
        if (isspace((unsigned char)c))
        {
            if (b->text_len > 0 && b->text_scratch[b->text_len - 1] == ' ')
                continue;
            c = ' ';
        }
        if (b->text_len + 1 > b->text_cap)
        {
            bld_grow(b, (void**)&b->text_scratch, &b->text_cap, 1, b->text_len + 64);
            if (b->error)
                return;
        }
        b->text_scratch[b->text_len++] = c;
    }
}

static void bld_set_param(svg_bld* b, uint32_t slot, float value)
{
    if (slot >= 11)
        return;
    if (slot >= b->nparams)
        b->nparams = slot + 1;
    b->p[slot] = value;
    b->param_mask |= 1u << slot;
    if (b->count && b->depth > 0 && b->nparams > b->tmp[b->stack[b->depth - 1]].num_params)
        b->tmp[b->stack[b->depth - 1]].num_params = (uint16_t)b->nparams;
}

static void bld_attr(svg_bld* b, String_View name, String_View raw)
{
    if (b->depth == 0)
        return;
    uint32_t idx = b->stack[b->depth - 1];
    uint32_t type = b->count ? b->tmp[idx].type : b->doc->elements[idx].type;

    if (raw.count + 1 > b->vbuf_cap)
    {
        bld_grow(b, (void**)&b->vbuf, &b->vbuf_cap, 1, raw.count + 1);
        if (b->error)
            return;
    }
    size_t len = svg_decode_entities(raw, b->vbuf, b->vbuf_cap - 1);
    b->vbuf[len] = '\0';
    String_View value = sv_from_parts(b->vbuf, len);
    value = sv_trim(value);
    float f;

    if (sv_eq(name, SV("style")))
    {
        svg_style_set_attr(&b->flags, &b->inl, &b->inl_mask, value);
        return;
    }

    if (sv_eq(name, SV("transform")))
    {
        svg_parse_transforms(&b->flags, value, &b->xform);
        return;
    }

    if (sv_eq(name, SV("id")))
    {
        if (b->count)
        {
            b->num_strings += (uint32_t)value.count;
            b->num_ids++;
            return;
        }
        uint32_t off = bld_strings(b, value);
        b->doc->id_elems[b->num_ids] = idx;
        b->doc->id_off[b->num_ids] = off;
        b->doc->id_len[b->num_ids] = (uint32_t)value.count;
        b->doc->elements[idx].id = b->num_ids;
        b->num_ids++;
        return;
    }

    if (sv_eq(name, SV("class")))
    {
        if (b->count)
        {
            b->num_strings += (uint32_t)value.count;
            return;
        }
        b->doc->elements[idx].class_off = bld_strings(b, value);
        b->doc->elements[idx].class_len = (uint32_t)value.count;
        return;
    }

    if (sv_eq(name, SV("href")) || (value.count > 0 && sv_eq_ignorecase(name, SV("xlink:href"))))
    {
        if (b->count)
        {
            b->num_strings += (uint32_t)value.count;
            return;
        }
        b->doc->elements[idx].href_off = bld_strings(b, value);
        b->doc->elements[idx].href_len = (uint32_t)value.count;
        return;
    }

    if (sv_eq(name, SV("clip-path")) && sv_starts_with(value, SV("url(")))
    {
        const char* hash = NULL;
        for (size_t k = 4; k + 1 < value.count; ++k)
        {
            if (value.data[k] == '#')
            {
                hash = value.data + k + 1;
                break;
            }
        }
        if (hash != NULL && hash[0] != '\0')
        {
            size_t end = (size_t)(value.data + value.count - hash);
            while (end > 0 && hash[end - 1] == ')')
                --end;
            String_View id = sv_from_parts(hash, end);
            if (b->count)
            {
                b->num_strings += (uint32_t)id.count;
                return;
            }
            b->doc->elements[idx].clip_off = bld_strings(b, id);
            b->doc->elements[idx].clip_len = (uint32_t)id.count;
        }
        return;
    }

    if (sv_eq(name, SV("viewBox")))
    {
        float v[4] = {0.f, 0.f, 0.f, 0.f};
        if (svg_parse_numbers(value, v, 4) == 4 && (type == svg_el_svg || type == svg_el_symbol))
        {
            bld_set_param(b, 4, v[0]);
            bld_set_param(b, 5, v[1]);
            bld_set_param(b, 6, v[2]);
            bld_set_param(b, 7, v[3]);
        }
        return;
    }

    if (sv_eq(name, SV("preserveAspectRatio")) && (type == svg_el_svg || type == svg_el_symbol))
    {
        uint8_t mode = 0, ax = 1, ay = 1;
        size_t w = 0;
        while (w < value.count && isspace((unsigned char)value.data[w]))
            ++w;
        size_t end = w;
        while (end < value.count && !isspace((unsigned char)value.data[end]))
            ++end;
        String_View first = sv_from_parts(value.data + w, end - w);
        if (sv_eq(first, SV("slice")))
            mode = 1;
        else if (sv_eq(first, SV("none")))
            mode = 2;
        if (end < value.count)
        {
            String_View second = sv_trim(sv_from_parts(value.data + end, value.count - end));
            if (sv_eq(second, SV("none")))
            {
                mode = 2;
            }
            else if (second.count >= 4)
            {
                ax = (uint8_t)(sv_eq(second, SV("xMin")) ? 0 : (sv_eq(second, SV("xMax")) ? 2 : 1));
                ay = (uint8_t)(sv_eq(second, SV("yMin")) ? 0 : (sv_eq(second, SV("yMax")) ? 2 : 1));
            }
        }
        bld_set_param(b, 8, (float)mode);
        bld_set_param(b, 9, (float)ax);
        bld_set_param(b, 10, (float)ay);
        return;
    }

    bool geometry = sv_eq(name, SV("x")) || sv_eq(name, SV("y")) || sv_eq(name, SV("width")) || sv_eq(name, SV("height")) ||
                    sv_eq(name, SV("rx")) || sv_eq(name, SV("ry")) || sv_eq(name, SV("cx")) || sv_eq(name, SV("cy")) ||
                    sv_eq(name, SV("r")) || sv_eq(name, SV("x1")) || sv_eq(name, SV("y1")) || sv_eq(name, SV("x2")) ||
                    sv_eq(name, SV("y2")) || sv_eq(name, SV("dx")) || sv_eq(name, SV("dy"));

    // geometry : a single number with an optional unit
    if (geometry && svg_parse_geom_value(value, &f))
    {
        if (sv_eq(name, SV("x")))
        {
            if (type == svg_el_rect) bld_set_param(b, 0, f);
            else if (type == svg_el_use) bld_set_param(b, 0, f);
            else if (type == svg_el_image) bld_set_param(b, 0, f);
            else if (type == svg_el_svg || type == svg_el_symbol) bld_set_param(b, 0, f);
            else if (type == svg_el_text) { bld_set_param(b, 0, f); b->text_mask |= 1; }
            else if (type == svg_el_tspan) { bld_set_param(b, 0, f); b->text_mask |= 1; }
        }
        else if (sv_eq(name, SV("y")))
        {
            if (type == svg_el_rect) bld_set_param(b, 1, f);
            else if (type == svg_el_use) bld_set_param(b, 1, f);
            else if (type == svg_el_image) bld_set_param(b, 1, f);
            else if (type == svg_el_svg || type == svg_el_symbol) bld_set_param(b, 1, f);
            else if (type == svg_el_text) { bld_set_param(b, 1, f); b->text_mask |= 2; }
            else if (type == svg_el_tspan) { bld_set_param(b, 1, f); b->text_mask |= 2; }
        }
        else if (sv_eq(name, SV("width")))
        {
            if (type == svg_el_rect) bld_set_param(b, 2, f);
            else if (type == svg_el_image) bld_set_param(b, 2, f);
            else if (type == svg_el_use) bld_set_param(b, 2, f);
            else if (type == svg_el_svg || type == svg_el_symbol) bld_set_param(b, 2, f);
        }
        else if (sv_eq(name, SV("height")))
        {
            if (type == svg_el_rect) bld_set_param(b, 3, f);
            else if (type == svg_el_image) bld_set_param(b, 3, f);
            else if (type == svg_el_use) bld_set_param(b, 3, f);
            else if (type == svg_el_svg || type == svg_el_symbol) bld_set_param(b, 3, f);
        }
        else if (sv_eq(name, SV("rx")))
        {
            if (type == svg_el_rect) bld_set_param(b, 4, f);
            else if (type == svg_el_ellipse) bld_set_param(b, 2, f);
        }
        else if (sv_eq(name, SV("ry")))
        {
            if (type == svg_el_rect) bld_set_param(b, 5, f);
            else if (type == svg_el_ellipse) bld_set_param(b, 3, f);
        }
        else if (sv_eq(name, SV("cx")))
        {
            if (type == svg_el_circle) bld_set_param(b, 0, f);
            else if (type == svg_el_ellipse) bld_set_param(b, 0, f);
        }
        else if (sv_eq(name, SV("cy")))
        {
            if (type == svg_el_circle) bld_set_param(b, 1, f);
            else if (type == svg_el_ellipse) bld_set_param(b, 1, f);
        }
        else if (sv_eq(name, SV("r")))
        {
            if (type == svg_el_circle) bld_set_param(b, 2, f);
        }
        else if (sv_eq(name, SV("x1")))
        {
            if (type == svg_el_line) bld_set_param(b, 0, f);
        }
        else if (sv_eq(name, SV("y1")))
        {
            if (type == svg_el_line) bld_set_param(b, 1, f);
        }
        else if (sv_eq(name, SV("x2")))
        {
            if (type == svg_el_line) bld_set_param(b, 2, f);
        }
        else if (sv_eq(name, SV("y2")))
        {
            if (type == svg_el_line) bld_set_param(b, 3, f);
        }
        else if (sv_eq(name, SV("dx")))
        {
            if (type == svg_el_tspan) { bld_set_param(b, 2, f); b->text_mask |= 4; }
        }
        else if (sv_eq(name, SV("dy")))
        {
            if (type == svg_el_tspan) { bld_set_param(b, 3, f); b->text_mask |= 8; }
        }
        else
            return;

        if (b->count)
        {
            if (b->nparams > b->tmp[idx].num_params)
                b->tmp[idx].num_params = (uint16_t)b->nparams;
        }
        return;
    }

    if (sv_eq(name, SV("points")) && (type == svg_el_polygon || type == svg_el_polyline))
    {
        bld_points(b, idx, value);
        return;
    }

    if (sv_eq(name, SV("d")) && type == svg_el_path)
    {
        bld_path_d(b, idx, value);
        return;
    }

    // everything else is a presentation attribute
    svg_style_set_prop(&b->flags, &b->pres, &b->pres_mask, name, value);
}

static void bld_run(svg_bld* b, String_View text)
{
    String_View sv = text;
    for (;;)
    {
        svg_token t = svg_xml_next(&sv);
        if (t.type == svg_tok_end)
            break;
        switch (t.type)
        {
        case svg_tok_open:
        {
            uint32_t idx = bld_begin(b, t.data);
            if (b->error)
                return;
            bool self = false;
            for (;;)
            {
                svg_token a = svg_xml_attr_or_end(&sv);
                if (a.type == svg_tok_close || a.type == svg_tok_end)
                    break;
                if (a.type == svg_tok_self)
                {
                    self = true;
                    break;
                }
                if (a.type != svg_tok_attr)
                    continue;
                svg_token v = svg_xml_value(&sv);
                if (v.type == svg_tok_value)
                    bld_attr(b, a.data, v.data);
                if (b->error)
                    return;
            }
            bld_snapshot(b, idx);
            if (self)
                bld_end(b, SV_NULL);
            break;
        }
        case svg_tok_close:
            break;
        case svg_tok_endtag:
            bld_end(b, t.data);
            break;
        case svg_tok_text:
            bld_text(b, t.data);
            break;
        case svg_tok_skip:
            break;
        default:
            break;
        }
    }
}

//----------------------------------------------------------------------------------------------------------------------------
// rules (CSS subset : simple type / class / id selectors)

static bool svg_el_has_class(const svg_document* doc, const svg_element* e, String_View cls)
{
    String_View c = sv_from_parts(doc->strings + e->class_off, e->class_len);
    for (;;)
    {
        c = sv_trim_left(c);
        if (c.count == 0)
            return false;
        size_t end = 0;
        while (end < c.count && !isspace((unsigned char)c.data[end]))
            ++end;
        if (sv_eq(sv_from_parts(c.data, end), cls))
            return true;
        c = sv_from_parts(c.data + end, c.count - end);
    }
}

static bool svg_el_id_is(const svg_document* doc, const svg_element* e, String_View id)
{
    if (e->id == SVG_INVALID)
        return false;
    return sv_eq(sv_from_parts(doc->strings + doc->id_off[e->id], doc->id_len[e->id]), id);
}

// one simple selector : "*", or a type name with any number of .class / #id parts
static bool svg_simple_selector_match(const svg_document* doc, const svg_element* e, String_View sel)
{
    sel = sv_trim(sel);
    if (sel.count == 0)
        return false;
    if (sv_eq(sel, SV("*")))
        return true;

    bool ok = true;
    String_View s = sel;
    for (;;)
    {
        s = sv_trim_left(s);
        if (s.count == 0)
            break;
        if (s.data[0] == '.')
        {
            String_View rest = sv_from_parts(s.data + 1, s.count - 1);
            size_t end = 0;
            while (end < rest.count && rest.data[end] != '.' && rest.data[end] != '#')
                ++end;
            ok = ok && svg_el_has_class(doc, e, sv_from_parts(rest.data, end));
            s = sv_from_parts(rest.data + end, rest.count - end);
        }
        else if (s.data[0] == '#')
        {
            String_View rest = sv_from_parts(s.data + 1, s.count - 1);
            size_t end = 0;
            while (end < rest.count && rest.data[end] != '.' && rest.data[end] != '#')
                ++end;
            ok = ok && svg_el_id_is(doc, e, sv_from_parts(rest.data, end));
            s = sv_from_parts(rest.data + end, rest.count - end);
        }
        else
        {
            size_t end = 0;
            while (end < s.count && s.data[end] != '.' && s.data[end] != '#')
                ++end;
            ok = ok && sv_eq(sv_from_parts(s.data, end), sv_from_parts(doc->strings + e->name_off, e->name_len));
            s = sv_from_parts(s.data + end, s.count - end);
        }
    }
    return ok;
}

static bool svg_rule_matches(const svg_document* doc, const svg_element* e, String_View list)
{
    String_View rest = list;
    for (;;)
    {
        size_t comma = 0;
        bool found = false;
        for (size_t k = 0; k < rest.count; ++k)
        {
            if (rest.data[k] == ',')
            {
                comma = k;
                found = true;
                break;
            }
        }
        String_View sel = found ? sv_from_parts(rest.data, comma) : rest;
        if (svg_simple_selector_match(doc, e, sel))
            return true;
        if (!found)
            return false;
        rest = sv_from_parts(rest.data + comma + 1, rest.count - comma - 1);
    }
}

static uint32_t svg_rule_specificity(String_View sel)
{
    uint32_t spec = 0;
    for (size_t k = 0; k < sel.count; ++k)
    {
        char c = sel.data[k];
        if (c == '#')
            spec += 100;
        else if (c == '.')
            spec += 10;
        else if (isalpha((unsigned char)c) && (k == 0 || isspace((unsigned char)sel.data[k - 1]) || sel.data[k - 1] == ','))
            spec += 1;
    }
    return spec;
}

// one "selector-list { declarations }" block ; counted in pass 1, stored in pass 2
static void svg_parse_rule(svg_bld* b, String_View rule)
{
    size_t open = 0;
    while (open < rule.count && rule.data[open] != '{')
        ++open;
    if (open >= rule.count)
        return;
    String_View sel = sv_trim(sv_from_parts(rule.data, open));
    size_t close = open + 1;
    while (close < rule.count && rule.data[close] != '}')
        ++close;
    String_View body = sv_from_parts(rule.data + open + 1, close - open - 1);

    if (sel.count == 0)
        return;

    svg_style props = {0};
    uint32_t mask = 0;
    {
        String_View d = body;
        for (;;)
        {
            size_t semi = 0;
            bool found = false;
            for (size_t k = 0; k < d.count; ++k)
            {
                if (d.data[k] == ';')
                {
                    semi = k;
                    found = true;
                    break;
                }
            }
            String_View decl = found ? sv_from_parts(d.data, semi) : d;
            if (sv_trim(decl).count > 0)
                svg_style_set_decl(&b->flags, &props, &mask, decl);
            if (!found)
                break;
            d = sv_from_parts(d.data + semi + 1, d.count - semi - 1);
        }
    }
    if (mask == 0)
        return;

    if (b->count)
    {
        b->num_rules++;
        b->num_rule_strs += (uint32_t)sel.count;
        return;
    }

    svg_rule* r = &b->doc->rules[b->num_rules];
    r->str_off = b->rule_strs_off;
    r->str_len = (uint32_t)sel.count;
    memcpy(b->doc->rule_strs + b->rule_strs_off, sel.data, sel.count);
    b->rule_strs_off += (uint32_t)sel.count;
    r->spec = svg_rule_specificity(sel);
    r->mask = mask;
    r->props = props;
    b->num_rules++;
}

// all the rule blocks of one <style> element
static void svg_parse_rules(svg_bld* b, String_View text)
{
    for (;;)
    {
        for (;;)
        {
            text = sv_trim_left(text);
            if (sv_starts_with(text, SV("/*")))
            {
                size_t end = 0;
                while (end + 1 < text.count && !(text.data[end] == '*' && text.data[end + 1] == '/'))
                    ++end;
                if (end + 1 < text.count)
                    text = sv_from_parts(text.data + end + 2, text.count - end - 2);
                else
                    return;
                continue;
            }
            break;
        }
        if (text.count == 0)
            return;
        size_t open = 0;
        while (open < text.count && text.data[open] != '{')
            ++open;
        if (open >= text.count)
            return;
        size_t close = open + 1;
        while (close < text.count && text.data[close] != '}')
            ++close;
        if (close >= text.count)
            return;
        svg_parse_rule(b, sv_from_parts(text.data, close + 1));
        text = sv_from_parts(text.data + close + 1, text.count - close - 1);
    }
}

// the cascade for one element : inherited, then the matching rules by (specificity, order),
// then the presentation attributes, then the style attribute ; currentColor resolves last
static void svg_resolve_style(const svg_document* doc, uint32_t idx, const svg_style* inherited, svg_style* out)
{
    const svg_element* e = &doc->elements[idx];
    *out = inherited ? *inherited : svg_style_defaults();

    for (uint32_t i = 0; i < doc->num_rules; ++i)
    {
        const svg_rule* rule = &doc->rules[i];
        if (svg_rule_matches(doc, e, sv_from_parts(doc->rule_strs + rule->str_off, rule->str_len)))
            svg_style_apply_mask(out, &rule->props, rule->mask);
    }
    svg_style_apply_mask(out, &doc->pres[idx], doc->pres_mask[idx]);
    svg_style_apply_mask(out, &doc->inl[idx], doc->inl_mask[idx]);

    if (out->fill_current)
    {
        out->fill = out->color;
        out->fill_current = false;
    }
    if (out->stroke_current)
    {
        out->stroke = out->color;
        out->stroke_current = false;
    }
}

//----------------------------------------------------------------------------------------------------------------------------
// public api

static void svg_free_all(svg_mem* m, svg_document* doc)
{
    svg_mem_free(m, doc->elements);
    svg_mem_free(m, doc->pres);
    svg_mem_free(m, doc->pres_mask);
    svg_mem_free(m, doc->inl);
    svg_mem_free(m, doc->inl_mask);
    svg_mem_free(m, doc->transforms);
    svg_mem_free(m, doc->params);
    svg_mem_free(m, doc->points);
    svg_mem_free(m, doc->paths);
    svg_mem_free(m, doc->text);
    svg_mem_free(m, doc->strings);
    svg_mem_free(m, doc->rule_strs);
    svg_mem_free(m, doc->rules);
    svg_mem_free(m, doc->id_elems);
    svg_mem_free(m, doc->id_off);
    svg_mem_free(m, doc->id_len);
    svg_mem_free(m, doc);
}

svg_document* svg_parse(String_View text, svg_allocator allocator)
{
    svg_bld b;
    memset(&b, 0, sizeof(b));
    b.mem = (svg_mem){allocator};
    b.count = true;

    bld_run(&b, text);
    if (b.error || b.num_elements == 0)
        return NULL;

    svg_document* doc = (svg_document*)svg_mem_alloc(&b.mem, sizeof(svg_document), sizeof(void*));
    if (doc == NULL)
        return NULL;
    memset(doc, 0, sizeof(*doc));
    doc->alloc = allocator;
    doc->flags = b.flags;
    doc->root = 0;
    doc->root_width = (b.root_w > 0.f) ? b.root_w : 100.f;
    doc->root_height = (b.root_h > 0.f) ? b.root_h : 100.f;
    doc->num_elements = b.num_elements;
    doc->num_params = b.num_params;
    doc->num_points = b.num_points;
    doc->num_paths = b.num_path;
    doc->num_text = b.num_text;
    doc->num_strings = b.num_strings;
    doc->num_rule_strs = b.num_rule_strs;
    doc->num_rules = b.num_rules;
    doc->num_ids = b.num_ids;

    bool ok = true;
    if (b.num_elements > 0)
    {
        doc->elements = (svg_element*)svg_mem_alloc(&b.mem, (size_t)b.num_elements * sizeof(svg_element), sizeof(void*));
        doc->pres = (svg_style*)svg_mem_alloc(&b.mem, (size_t)b.num_elements * sizeof(svg_style), sizeof(void*));
        doc->pres_mask = (uint32_t*)svg_mem_alloc(&b.mem, (size_t)b.num_elements * sizeof(uint32_t), sizeof(uint32_t));
        doc->inl = (svg_style*)svg_mem_alloc(&b.mem, (size_t)b.num_elements * sizeof(svg_style), sizeof(void*));
        doc->inl_mask = (uint32_t*)svg_mem_alloc(&b.mem, (size_t)b.num_elements * sizeof(uint32_t), sizeof(uint32_t));
        doc->transforms = (float*)svg_mem_alloc(&b.mem, (size_t)b.num_elements * 6 * sizeof(float), sizeof(float));
        ok = doc->elements != NULL && doc->pres != NULL && doc->pres_mask != NULL && doc->inl != NULL && doc->inl_mask != NULL &&
             doc->transforms != NULL;
    }
    if (ok && b.num_params > 0)
        ok = (doc->params = (float*)svg_mem_alloc(&b.mem, (size_t)b.num_params * sizeof(float), sizeof(float))) != NULL;
    if (ok && b.num_points > 0)
        ok = (doc->points = (float*)svg_mem_alloc(&b.mem, (size_t)b.num_points * sizeof(float), sizeof(float))) != NULL;
    if (ok && b.num_path > 0)
        ok = (doc->paths = (svg_path_cmd*)svg_mem_alloc(&b.mem, (size_t)b.num_path * sizeof(svg_path_cmd), sizeof(void*))) != NULL;
    if (ok && b.num_text > 0)
        ok = (doc->text = (char*)svg_mem_alloc(&b.mem, (size_t)b.num_text + 1, 1)) != NULL;
    if (ok && b.num_strings > 0)
        ok = (doc->strings = (char*)svg_mem_alloc(&b.mem, (size_t)b.num_strings, 1)) != NULL;
    if (ok && b.num_rules > 0)
    {
        doc->rules = (svg_rule*)svg_mem_alloc(&b.mem, (size_t)b.num_rules * sizeof(svg_rule), sizeof(void*));
        doc->rule_strs = (char*)svg_mem_alloc(&b.mem, (size_t)b.num_rule_strs, 1);
        ok = doc->rules != NULL && doc->rule_strs != NULL;
    }
    if (ok && b.num_ids > 0)
    {
        doc->id_elems = (uint32_t*)svg_mem_alloc(&b.mem, (size_t)b.num_ids * sizeof(uint32_t), sizeof(uint32_t));
        doc->id_off = (uint32_t*)svg_mem_alloc(&b.mem, (size_t)b.num_ids * sizeof(uint32_t), sizeof(uint32_t));
        doc->id_len = (uint32_t*)svg_mem_alloc(&b.mem, (size_t)b.num_ids * sizeof(uint32_t), sizeof(uint32_t));
        ok = doc->id_elems != NULL && doc->id_off != NULL && doc->id_len != NULL;
    }
    if (!ok)
    {
        svg_free_all(&b.mem, doc);
        return NULL;
    }

    // drop the counting pass scratch, the write pass starts from a clean builder
    if (b.tmp) svg_mem_free(&b.mem, b.tmp);
    if (b.style_text) svg_mem_free(&b.mem, b.style_text);
    if (b.text_scratch) svg_mem_free(&b.mem, b.text_scratch);
    if (b.vbuf) svg_mem_free(&b.mem, b.vbuf);

    svg_bld w;
    memset(&w, 0, sizeof(w));
    w.mem = b.mem;
    w.doc = doc;
    bld_run(&w, text);
    // the write pass must land exactly on the counting pass's totals
    if (w.error || w.num_elements != doc->num_elements || w.params_off != doc->num_params ||
        w.points_off != doc->num_points || w.path_off != doc->num_paths || w.text_off != doc->num_text ||
        w.strings_off != doc->num_strings || w.rule_strs_off != doc->num_rule_strs || w.num_rules != doc->num_rules ||
        w.num_ids != doc->num_ids)
    {
        svg_free_all(&b.mem, doc);
        return NULL;
    }

    // the cascade applies the rules by ascending (specificity, document order) : a stable sort on specificity
    for (uint32_t i = 1; i < doc->num_rules; ++i)
    {
        svg_rule r = doc->rules[i];
        uint32_t j = i;
        while (j > 0 && doc->rules[j - 1].spec > r.spec)
        {
            doc->rules[j] = doc->rules[j - 1];
            --j;
        }
        doc->rules[j] = r;
    }
    return doc;
}

void svg_free_document(svg_document* doc, svg_allocator allocator)
{
    if (doc == NULL)
        return;
    svg_mem m = {allocator};
    svg_free_all(&m, doc);
}

void svg_document_size(const svg_document* doc, float* width, float* height)
{
    if (width)
        *width = doc->root_width;
    if (height)
        *height = doc->root_height;
}

uint32_t svg_document_flags(const svg_document* doc)
{
    return doc->flags;
}

//----------------------------------------------------------------------------------------------------------------------------
// draw list

typedef enum svg_cmd_kind
{
    svg_cmd_polygon,
    svg_cmd_line,
    svg_cmd_capsule,
    svg_cmd_box,
    svg_cmd_oriented_box,
    svg_cmd_oriented_rect,
    svg_cmd_disc,
    svg_cmd_ring,
    svg_cmd_ellipse,
    svg_cmd_ellipse_ring,
    svg_cmd_dashed_line,
    svg_cmd_dashed_ring,
    svg_cmd_dashed_box,
    svg_cmd_dashed_oriented_box,
    svg_cmd_dashed_ellipse_ring,
    svg_cmd_text,
    svg_cmd_clip_rect,
    svg_cmd_clip_rect_rounded,
    svg_cmd_clip_disc,
    svg_cmd_clip_pop
} svg_cmd_kind;

typedef struct svg_cmd
{
    uint32_t data_off;
    uint32_t str_off;     // text only : offset into the list strings
    uint32_t str_len;
    uint32_t color;
    float box[4];         // min x, min y, max x, max y in prepared space
    uint8_t kind;
    uint16_t num_data;
} svg_cmd;

struct svg_draw_list
{
    svg_allocator alloc;
    uint32_t num_commands;
    uint32_t num_data;
    uint32_t num_strings;
    svg_cmd* cmds;
    float* data;
    char* strings;
};

typedef struct svg_prep
{
    svg_document* doc;
    svg_draw_list* list;
    bool count;
    bool error;
    float tolerance;

    uint32_t num_commands;
    uint32_t num_data;
    uint32_t num_strings;
    uint32_t data_off;
    uint32_t strings_off;

    float verts[OD_MAX_POLYGON_VERTICES * 2];
    uint32_t num_verts;
    bool oversized;
} svg_prep;

// ---- flattening ----

static void prep_point(svg_prep* p, const svg_xform* m, float x, float y)
{
    if (p->oversized)
        return;
    if (p->num_verts >= OD_MAX_POLYGON_VERTICES * 2)
    {
        p->oversized = true;
        return;
    }
    p->verts[p->num_verts++] = m->a * x + m->c * y + m->e;
    p->verts[p->num_verts++] = m->b * x + m->d * y + m->f;
}

static float prep_scale(const svg_xform* m)
{
    float sx = sqrtf(m->a * m->a + m->b * m->b);
    float sy = sqrtf(m->c * m->c + m->d * m->d);
    float s = fmaxf(sx, sy);
    return s > 1e-6f ? s : 1e-6f;
}

static void prep_cubic(svg_prep* p, const svg_xform* m, float x0, float y0, const float v[6])
{
    float scale = prep_scale(m);
    // one frame per pending cubic : p0, p1, p2, p3
    float stack[64];
    uint32_t top = 0;
    stack[top++] = x0;
    stack[top++] = y0;
    for (uint32_t k = 0; k < 6; ++k)
        stack[top++] = v[k];

    while (top > 0 && !p->oversized)
    {
        top -= 8;
        const float* f = stack + top;
        // distance of the curve midpoint to the chord midpoint, a bound on the max deviation / 4
        float mx = .125f * (f[0] + 3.f * (f[2] + f[4]) + f[6]);
        float my = .125f * (f[1] + 3.f * (f[3] + f[5]) + f[7]);
        float err = fabsf(mx - (f[0] + f[6]) * .5f) + fabsf(my - (f[1] + f[7]) * .5f);
        if (err * 4.f * scale <= p->tolerance || top >= 40)
        {
            prep_point(p, m, f[6], f[7]);
            continue;
        }
        float a1x = (f[0] + f[2]) * .5f, a1y = (f[1] + f[3]) * .5f;
        float a2x = (f[2] + f[4]) * .5f, a2y = (f[3] + f[5]) * .5f;
        float a3x = (f[4] + f[6]) * .5f, a3y = (f[5] + f[7]) * .5f;
        float b1x = (a1x + a2x) * .5f, b1y = (a1y + a2y) * .5f;
        float b2x = (a2x + a3x) * .5f, b2y = (a2y + a3y) * .5f;
        float midx = (b1x + b2x) * .5f, midy = (b1y + b2y) * .5f;
        // the right half first, so the left one is popped first
        stack[top++] = midx;
        stack[top++] = midy;
        stack[top++] = b2x;
        stack[top++] = b2y;
        stack[top++] = a3x;
        stack[top++] = a3y;
        stack[top++] = f[6];
        stack[top++] = f[7];
        stack[top++] = f[0];
        stack[top++] = f[1];
        stack[top++] = a1x;
        stack[top++] = a1y;
        stack[top++] = b1x;
        stack[top++] = b1y;
        stack[top++] = midx;
        stack[top++] = midy;
    }
}

static void prep_quad(svg_prep* p, const svg_xform* m, float x0, float y0, const float v[4])
{
    float c[6];
    c[0] = x0 + (2.f / 3.f) * (v[0] - x0);
    c[1] = y0 + (2.f / 3.f) * (v[1] - y0);
    c[2] = v[2] + (2.f / 3.f) * (v[0] - v[2]);
    c[3] = v[3] + (2.f / 3.f) * (v[1] - v[3]);
    c[4] = v[2];
    c[5] = v[3];
    prep_cubic(p, m, x0, y0, c);
}

// [rx, ry, x_axis_rotation_deg, large_arc, sweep, x, y]
static void prep_arc(svg_prep* p, const svg_xform* m, float x0, float y0, const float v[7])
{
    float x2 = v[5], y2 = v[6];
    float rx = fabsf(v[0]), ry = fabsf(v[1]);
    bool large = v[3] != 0.f;
    bool sweep = v[4] != 0.f;

    if (rx < 1e-6f || ry < 1e-6f)
    {
        prep_point(p, m, x2, y2);
        return;
    }

    float phi = (float)(v[2] * M_PI / 180.0);
    float cphi = cosf(phi), sphi = sinf(phi);

    // endpoint to center parameterization (SVG 1.1 F.6.5)
    float dx = (x0 - x2) * .5f, dy = (y0 - y2) * .5f;
    float x1p = cphi * dx + sphi * dy;
    float y1p = -sphi * dx + cphi * dy;

    float l = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
    if (l > 1.f)
    {
        float s = sqrtf(l);
        rx *= s;
        ry *= s;
    }

    float rx2 = rx * rx, ry2 = ry * ry;
    float num = rx2 * ry2 - rx2 * y1p * y1p - ry2 * x1p * x1p;
    float den = rx2 * y1p * y1p + ry2 * x1p * x1p;
    float co = (den > 0.f) ? sqrtf((num > 0.f ? num : 0.f) / den) : 0.f;
    if (large == sweep)
        co = -co;
    float cxp = co * (rx * y1p) / ry;
    float cyp = -co * (ry * x1p) / rx;
    float cx = cphi * cxp - sphi * cyp + (x0 + x2) * .5f;
    float cy = sphi * cxp + cphi * cyp + (y0 + y2) * .5f;

    float u = x1p / rx, w = y1p / ry;
    float u2 = -(x1p + cxp) / rx, w2 = -(y1p + cyp) / ry;
    float theta1 = (float)atan2(w, u);
    float dtheta = (float)atan2(u * w2 - w * u2, u * u2 + w * w2);
    if (!sweep && dtheta > 0.f)
        dtheta -= 2.f * (float)M_PI;
    if (sweep && dtheta < 0.f)
        dtheta += 2.f * (float)M_PI;

    // one cubic per arc of at most 90deg (SVG 1.1 F.6.6)
    uint32_t n = (uint32_t)ceilf(fabsf(dtheta) / ((float)M_PI * .5f));
    if (n == 0)
        n = 1;
    float dt = dtheta / (float)n;
    float k = 4.f / 3.f * tanf(dt * .25f);

    for (uint32_t i = 0; i < n && !p->oversized; ++i)
    {
        float a0 = theta1 + (float)i * dt;
        float a1 = theta1 + (float)(i + 1) * dt;
        float c0 = cosf(a0), s0 = sinf(a0);
        float c1 = cosf(a1), s1 = sinf(a1);
        float px0 = cx + cphi * rx * c0 - sphi * ry * s0;
        float py0 = cy + sphi * rx * c0 + cphi * ry * s0;
        float px1 = cx + cphi * rx * c1 - sphi * ry * s1;
        float py1 = cy + sphi * rx * c1 + cphi * ry * s1;
        float dpx0 = -cphi * rx * s0 - sphi * ry * c0;
        float dpy0 = -sphi * rx * s0 + cphi * ry * c0;
        float dpx1 = -cphi * rx * s1 - sphi * ry * c1;
        float dpy1 = -sphi * rx * s1 + cphi * ry * c1;
        float c[6];
        c[0] = px0 + k * dpx0;
        c[1] = py0 + k * dpy0;
        c[2] = px1 - k * dpx1;
        c[3] = py1 - k * dpy1;
        c[4] = px1;
        c[5] = py1;
        prep_cubic(p, m, px0, py0, c);
    }
}

// samples an ellipse centered at (cx, cy) with radii (rx, ry) into a polygon
static void prep_ellipse_poly(svg_prep* p, const svg_xform* m, float cx, float cy, float rx, float ry)
{
    float r = (fabsf(rx) > fabsf(ry)) ? fabsf(rx) : fabsf(ry);
    r *= prep_scale(m);
    uint32_t n = (uint32_t)ceilf(sqrtf((float)M_PI * (float)M_PI * r / (2.f * p->tolerance)));
    if (n < 8)
        n = 8;
    if (n > OD_MAX_POLYGON_VERTICES)
    {
        p->oversized = true;
        return;
    }
    for (uint32_t i = 0; i < n; ++i)
    {
        float a = (float)i / (float)n * 2.f * (float)M_PI;
        prep_point(p, m, cx + rx * cosf(a), cy + ry * sinf(a));
    }
}

// ---- command emission ----

static void prep_cmd(svg_prep* p, uint8_t kind, uint32_t num_data, const float* box, uint32_t color)
{
    if (p->count)
    {
        p->num_commands++;
        p->num_data += num_data;
        return;
    }
    svg_draw_list* l = p->list;
    svg_cmd* c = &l->cmds[l->num_commands];
    c->kind = kind;
    c->num_data = (uint8_t)num_data;
    c->data_off = p->data_off;
    c->str_off = 0;
    c->str_len = 0;
    c->color = color;
    memcpy(c->box, box, sizeof(c->box));
    p->data_off += num_data;
    l->num_commands++;
}

static void prep_cmd_data(svg_prep* p, uint8_t kind, const float* d, uint32_t num_data, const float* box, uint32_t color)
{
    if (box == NULL)
    {
        float zero[4] = {0.f, 0.f, 0.f, 0.f};
        box = zero;
    }
    prep_cmd(p, kind, num_data, box, color);
    if (!p->count && num_data > 0)
        memcpy(p->list->data + p->data_off - num_data, d, num_data * sizeof(float));
}

// closes the current piece into a polygon command (auto-close, no fill rule beyond parity)
static void prep_emit_poly(svg_prep* p, uint32_t color)
{
    uint32_t nv = p->num_verts / 2;
    p->num_verts = 0;
    if (p->oversized)
    {
        p->oversized = false;
        p->doc->flags |= svg_flag_oversized_piece;
        return;
    }
    if (nv < 3)
        return;
    float box[4];
    box[0] = box[2] = p->verts[0];
    box[1] = box[3] = p->verts[1];
    for (uint32_t i = 2; i < nv * 2; i += 2)
    {
        if (p->verts[i] < box[0]) box[0] = p->verts[i];
        if (p->verts[i + 1] < box[1]) box[1] = p->verts[i + 1];
        if (p->verts[i] > box[2]) box[2] = p->verts[i];
        if (p->verts[i + 1] > box[3]) box[3] = p->verts[i + 1];
    }
    if (p->count)
    {
        p->num_commands++;
        p->num_data += nv * 2 + 1;
        return;
    }
    svg_draw_list* l = p->list;
    svg_cmd* c = &l->cmds[l->num_commands];
    c->kind = svg_cmd_polygon;
    c->num_data = (uint8_t)(nv * 2 + 1);
    c->data_off = p->data_off;
    c->str_off = 0;
    c->str_len = 0;
    c->color = color;
    memcpy(c->box, box, sizeof(box));
    float* data = l->data + p->data_off;
    data[0] = (float)nv;
    memcpy(data + 1, p->verts, nv * 2 * sizeof(float));
    p->data_off += nv * 2 + 1;
    l->num_commands++;
}

// the state of the element stack : cumulative transform, its classification, accumulated opacity,
// the <use> instance stack (cycle check) and the viewBox-mapped marker
typedef struct svg_ctx
{
    svg_xform m;
    uint32_t cls;
    float uniform_scale;
    float opacity;
    bool via_use;
    uint32_t use_stack[64];
    uint32_t use_depth;
} svg_ctx;

static uint32_t prep_color(const svg_style* st, const svg_ctx* c, bool fill)
{
    uint32_t col = fill ? st->fill : st->stroke;
    float a = ((col >> 24) & 0xff) / 255.f * c->opacity;
    a *= fill ? st->fill_opacity : st->stroke_opacity;
    if (a > 1.f)
        a = 1.f;
    return svg_color_alpha(col, a);
}

// ---- primitive emitters ----

static void prep_stroke_run(svg_prep* p, const svg_ctx* c, const svg_style* st, uint32_t color, bool close);

static void prep_draw_rect(svg_prep* p, const svg_ctx* c, float x, float y, float w, float h, float rx, float ry,
                           bool fill, uint32_t fill_color, uint32_t stroke_color, float stroke_width, bool dashed,
                           float dash, float gap, float offset, bool round_caps)
{
    if (w <= 0.f || h <= 0.f)
        return;
    float r = (rx > 0.f && ry > 0.f) ? (rx + ry) * .5f : fmaxf(rx, ry);
    if (r > w * .5f)
        r = w * .5f;
    if (r > h * .5f)
        r = h * .5f;

    bool axis_aligned = c->m.b == 0.f && c->m.c == 0.f;
    svg_pt q0 = svg_xform_pt(&c->m, x, y);
    svg_pt q1 = svg_xform_pt(&c->m, x + w, y + h);
    float box[4] = {fminf(q0.x, q1.x), fminf(q0.y, q1.y), fmaxf(q0.x, q1.x), fmaxf(q0.y, q1.y)};
    svg_pt a = svg_xform_pt(&c->m, x + r, y + h * .5f);
    svg_pt b = svg_xform_pt(&c->m, x + w - r, y + h * .5f);
    float inner_h = (h - r * 2.f) * c->uniform_scale;
    float scaled_r = r * c->uniform_scale;

    if (fill)
    {
        if (c->cls == 0 || (c->cls == 1 && axis_aligned))
        {
            float d[5] = {box[0], box[1], box[2], box[3], scaled_r};
            prep_cmd_data(p, svg_cmd_box, d, 5, box, fill_color);
        }
        else if (c->cls == 1)
        {
            float d[6] = {a.x, a.y, b.x, b.y, inner_h, scaled_r};
            prep_cmd_data(p, svg_cmd_oriented_box, d, 6, box, fill_color);
        }
        else
        {
            const float pts[8] = {x, y, x + w, y, x + w, y + h, x, y + h};
            for (uint32_t i = 0; i < 8; i += 2)
                prep_point(p, &c->m, pts[i], pts[i + 1]);
            prep_emit_poly(p, fill_color);
        }
    }

    if (stroke_width <= 0.f)
        return;
    if (c->cls > 1)
    {
        p->doc->flags |= svg_flag_stroke_unsupported;
        return;
    }

    p->num_verts = 0;
    p->oversized = false;
    if (r > 0.f)
    {
        const float cx[4] = {x + r, x + w - r, x + w - r, x + r};
        const float cy[4] = {y + r, y + r, y + h - r, y + h - r};
        const float a0[4] = {3.14159265f, 4.71238898f, 0.f, 1.57079633f};
        for (uint32_t k = 0; k < 4; ++k)
        {
            for (uint32_t j = 0; j <= 4; ++j)
            {
                float a = a0[k] + (float)j * 1.57079633f * .25f;
                prep_point(p, &c->m, cx[k] + r * cosf(a), cy[k] + r * sinf(a));
            }
        }
    }
    else
    {
        prep_point(p, &c->m, x, y);
        prep_point(p, &c->m, x + w, y);
        prep_point(p, &c->m, x + w, y + h);
        prep_point(p, &c->m, x, y + h);
    }
    svg_style ss = {0};
    ss.stroke_width = stroke_width;
    ss.dash_set = dashed;
    ss.num_dash = dashed ? 2 : 0;
    ss.dash[0] = dash;
    ss.dash[1] = gap;
    ss.dash_offset = offset;
    ss.linecap = round_caps ? 1 : 0;
    prep_stroke_run(p, c, &ss, stroke_color, true);
}

static void prep_draw_circle(svg_prep* p, const svg_ctx* c, float cx, float cy, float r, bool fill,
                             uint32_t fill_color, uint32_t stroke_color, float stroke_width, bool dashed, float dash,
                             float gap, float offset, bool round_caps)
{
    if (r <= 0.f)
        return;
    if (fill)
    {
        if (c->cls <= 1)
        {
            svg_pt m = svg_xform_pt(&c->m, cx, cy);
            float rad = r * c->uniform_scale;
            float d[3] = {m.x, m.y, rad};
            float box[4] = {m.x - rad, m.y - rad, m.x + rad, m.y + rad};
            prep_cmd_data(p, svg_cmd_disc, d, 3, box, fill_color);
        }
        else
        {
            prep_ellipse_poly(p, &c->m, cx, cy, r, r);
            prep_emit_poly(p, fill_color);
        }
    }
    if (stroke_width <= 0.f)
        return;
    if (c->cls > 1)
    {
        p->doc->flags |= svg_flag_stroke_unsupported;
        return;
    }
    svg_pt m = svg_xform_pt(&c->m, cx, cy);
    float rad = r * c->uniform_scale;
    float t = stroke_width * c->uniform_scale;
    float box[4] = {m.x - rad - t, m.y - rad - t, m.x + rad + t, m.y + rad + t};
    if (dashed)
    {
        float d[8] = {m.x, m.y, rad, t, dash * c->uniform_scale, gap * c->uniform_scale, offset * c->uniform_scale,
                      (float)round_caps};
        prep_cmd_data(p, svg_cmd_dashed_ring, d, 8, box, stroke_color);
    }
    else
    {
        float d[4] = {m.x, m.y, rad, t};
        prep_cmd_data(p, svg_cmd_ring, d, 4, box, stroke_color);
    }
}

static void prep_draw_ellipse(svg_prep* p, const svg_ctx* c, float cx, float cy, float rx, float ry, bool fill,
                              uint32_t fill_color, uint32_t stroke_color, float stroke_width, bool dashed, float dash,
                              float gap, float offset, bool round_caps)
{
    if (rx <= 0.f || ry <= 0.f)
        return;
    // major axis endpoints in element space
    bool horizontal = fabsf(rx) >= fabsf(ry);
    float ax = horizontal ? cx - rx : cx;
    float ay = horizontal ? cy : cy - ry;
    float bx = horizontal ? cx + rx : cx;
    float by = horizontal ? cy : cy + ry;
    float width = horizontal ? ry * 2.f : rx * 2.f;

    if (fill)
    {
        if (c->cls <= 1)
        {
            svg_pt a = svg_xform_pt(&c->m, ax, ay);
            svg_pt b = svg_xform_pt(&c->m, bx, by);
            float d[5] = {a.x, a.y, b.x, b.y, width * c->uniform_scale};
            float box[4] = {fminf(a.x, b.x), fminf(a.y, b.y), fmaxf(a.x, b.x), fmaxf(a.y, b.y)};
            float w2 = width * c->uniform_scale * .5f;
            box[0] -= w2;
            box[1] -= w2;
            box[2] += w2;
            box[3] += w2;
            prep_cmd_data(p, svg_cmd_ellipse, d, 5, box, fill_color);
        }
        else
        {
            prep_ellipse_poly(p, &c->m, cx, cy, rx, ry);
            prep_emit_poly(p, fill_color);
        }
    }
    if (stroke_width <= 0.f)
        return;
    if (c->cls > 1)
    {
        p->doc->flags |= svg_flag_stroke_unsupported;
        return;
    }
    svg_pt a = svg_xform_pt(&c->m, ax, ay);
    svg_pt b = svg_xform_pt(&c->m, bx, by);
    float t = stroke_width * c->uniform_scale;
    float w2 = width * c->uniform_scale * .5f + t;
    float box[4] = {fminf(a.x, b.x) - w2, fminf(a.y, b.y) - w2, fmaxf(a.x, b.x) + w2, fmaxf(a.y, b.y) + w2};
    if (dashed)
    {
        float d[10] = {a.x, a.y, b.x, b.y, width * c->uniform_scale, t, dash * c->uniform_scale, gap * c->uniform_scale,
                       offset * c->uniform_scale, (float)round_caps};
        prep_cmd_data(p, svg_cmd_dashed_ellipse_ring, d, 10, box, stroke_color);
    }
    else
    {
        float d[6] = {a.x, a.y, b.x, b.y, width * c->uniform_scale, t};
        prep_cmd_data(p, svg_cmd_ellipse_ring, d, 6, box, stroke_color);
    }
}

static void prep_draw_line(svg_prep* p, const svg_ctx* c, float x1, float y1, float x2, float y2, float stroke_width,
                           uint8_t linecap, bool dashed, float dash, float gap, float offset, uint32_t color)
{
    if (stroke_width <= 0.f)
        return;
    if (c->cls > 1)
    {
        p->doc->flags |= svg_flag_stroke_unsupported;
        return;
    }
    svg_pt a = svg_xform_pt(&c->m, x1, y1);
    svg_pt b = svg_xform_pt(&c->m, x2, y2);
    float t = stroke_width * c->uniform_scale;

    if (dashed)
    {
        float d[9] = {a.x, a.y, b.x, b.y, t, dash * c->uniform_scale, gap * c->uniform_scale, offset * c->uniform_scale,
                      (float)(linecap == 1)};
        float box[4] = {fminf(a.x, b.x) - t, fminf(a.y, b.y) - t, fmaxf(a.x, b.x) + t, fmaxf(a.y, b.y) + t};
        prep_cmd_data(p, svg_cmd_dashed_line, d, 9, box, color);
        return;
    }

    if (linecap == 1)
    {
        float d[5] = {a.x, a.y, b.x, b.y, t * .5f};
        float box[4] = {fminf(a.x, b.x) - t * .5f, fminf(a.y, b.y) - t * .5f, fmaxf(a.x, b.x) + t * .5f,
                        fmaxf(a.y, b.y) + t * .5f};
        prep_cmd_data(p, svg_cmd_capsule, d, 5, box, color);
        return;
    }

    if (linecap == 2)
    {
        float dx = b.x - a.x, dy = b.y - a.y;
        float len = sqrtf(dx * dx + dy * dy);
        if (len > 1e-6f)
        {
            float ex = dx / len * t * .5f;
            float ey = dy / len * t * .5f;
            a.x -= ex;
            a.y -= ey;
            b.x += ex;
            b.y += ey;
        }
    }
    float d[5] = {a.x, a.y, b.x, b.y, t};
    float box[4] = {fminf(a.x, b.x) - t * .5f, fminf(a.y, b.y) - t * .5f, fmaxf(a.x, b.x) + t * .5f,
                    fmaxf(a.y, b.y) + t * .5f};
    prep_cmd_data(p, svg_cmd_line, d, 5, box, color);
}

static void prep_draw_text(svg_prep* p, const svg_ctx* c, float x, float y, const char* text, uint32_t len,
                           const svg_style* st)
{
    if (len == 0 || st->fill_none)
        return;
    float size = st->font_size;
    if (c->cls == 2 || (c->cls == 1 && (c->m.b != 0.f || c->m.c != 0.f)))
        p->doc->flags |= svg_flag_text_degraded;
    size *= c->cls <= 1 ? c->uniform_scale : (fabsf(c->m.a) + fabsf(c->m.c)) * .5f;
    if (size <= 0.f)
        return;
    svg_pt pos = svg_xform_pt(&c->m, x, y);
    uint32_t color = prep_color(st, c, true);

    uint32_t str_off;
    if (p->count)
        p->num_strings += len;
    else
    {
        str_off = p->strings_off;
        memcpy(p->list->strings + p->strings_off, text, len);
        p->strings_off += len;
    }

    // the width is not known without the renderer (od_string_view_width), estimate it for the culling box
    float width = (float)len * size * .6f;
    float box[4] = {pos.x, pos.y - size, pos.x + width, pos.y + size * .25f};
    if (st->anchor == 2)
    {
        box[2] = box[0];
        box[0] -= width;
    }
    else if (st->anchor == 1)
    {
        box[0] -= width * .5f;
        box[2] -= width * .5f;
    }

    if (p->count)
    {
        p->num_commands++;
        p->num_data += 5;
        return;
    }
    svg_draw_list* l = p->list;
    svg_cmd* cmd = &l->cmds[l->num_commands];
    cmd->kind = svg_cmd_text;
    cmd->num_data = 5;
    cmd->data_off = p->data_off;
    cmd->str_off = str_off;
    cmd->str_len = len;
    cmd->color = color;
    memcpy(cmd->box, box, sizeof(box));
    float* d = l->data + p->data_off;
    d[0] = pos.x;
    d[1] = pos.y;
    d[2] = size;
    d[3] = (float)st->font;
    d[4] = (float)st->anchor;
    p->data_off += 5;
    l->num_commands++;
}

//----------------------------------------------------------------------------------------------------------------------------
// walker

static float prep_param(const svg_document* doc, const svg_element* e, uint32_t i)
{
    if (i >= e->num_params)
        return 0.f;
    return doc->params[e->params + i];
}

static uint32_t prep_find_ref(const svg_document* doc, const svg_element* e)
{
    if (e->href_len == 0)
        return SVG_INVALID;
    String_View href = sv_trim(sv_from_parts(doc->strings + e->href_off, e->href_len));
    if (href.count == 0 || href.data[0] != '#')
        return SVG_INVALID;
    String_View id = sv_from_parts(href.data + 1, href.count - 1);
    for (uint32_t i = 0; i < doc->num_ids; ++i)
    {
        if (sv_eq(sv_from_parts(doc->strings + doc->id_off[i], doc->id_len[i]), id))
            return doc->id_elems[i];
    }
    return SVG_INVALID;
}

// The single rect/circle child of the clipPath referenced by [e], SVG_INVALID when the clip
// can't be mapped (unknown id, wrong type, more than one shape)
static uint32_t prep_clip_shape(const svg_document* doc, const svg_element* e)
{
    if (e->clip_len == 0)
        return SVG_INVALID;
    for (uint32_t i = 0; i < doc->num_ids; ++i)
    {
        if (sv_eq(sv_from_parts(doc->strings + doc->id_off[i], doc->id_len[i]),
                  sv_from_parts(doc->strings + e->clip_off, e->clip_len)))
        {
            const svg_element* cp = &doc->elements[doc->id_elems[i]];
            if (cp->type != svg_el_clip_path)
                return SVG_INVALID;
            uint32_t child = cp->first_child;
            if (child == SVG_INVALID || doc->elements[child].next_sibling != SVG_INVALID)
                return SVG_INVALID;
            uint32_t type = doc->elements[child].type;
            if (type != svg_el_rect && type != svg_el_circle)
                return SVG_INVALID;
            return child;
        }
    }
    return SVG_INVALID;
}

static void prep_push_clip(svg_prep* p, uint8_t kind, const float* d, uint32_t num_data, const float* box)
{
    prep_cmd_data(p, kind, d, num_data, box, 0);
}

static void prep_text_walk(svg_prep* p, const svg_ctx* c, const svg_element* e, const svg_style* st, float cx, float cy);

static void prep_stroke_run(svg_prep* p, const svg_ctx* c, const svg_style* st, uint32_t color, bool close)
{
    uint32_t n = p->num_verts / 2;
    if (p->oversized)
    {
        p->num_verts = 0;
        p->oversized = false;
        p->doc->flags |= svg_flag_oversized_piece;
        return;
    }
    if (n < 2 || c->cls > 1)
    {
        p->num_verts = 0;
        if (n >= 2)
            p->doc->flags |= svg_flag_stroke_unsupported;
        return;
    }

    float t = st->stroke_width * c->uniform_scale;
    if (t <= 0.f)
    {
        p->num_verts = 0;
        return;
    }

    uint32_t segments = close ? n : n - 1;
    float dash = st->dash[0] * c->uniform_scale;
    float gap = (st->num_dash > 1 ? st->dash[1] : st->dash[0]) * c->uniform_scale;
    bool dashed = st->dash_set && st->num_dash > 0 && dash > 0.f;
    float phase = st->dash_offset * c->uniform_scale;
    if (dashed && gap <= 0.f)
        dashed = false;
    if (dashed)
    {
        float pattern = dash + gap;
        phase = fmodf(phase, pattern);
        if (phase < 0.f)
            phase += pattern;
    }

    for (uint32_t i = 0; i < segments; ++i)
    {
        const float* a = p->verts + i * 2;
        const float* b = p->verts + ((i + 1) % n) * 2;
        float box[4] = {fminf(a[0], b[0]) - t * .5f, fminf(a[1], b[1]) - t * .5f, fmaxf(a[0], b[0]) + t * .5f,
                        fmaxf(a[1], b[1]) + t * .5f};
        if (dashed)
        {
            float d[9] = {a[0], a[1], b[0], b[1], t, dash, gap, phase, (float)(st->linecap == 1)};
            prep_cmd_data(p, svg_cmd_dashed_line, d, 9, box, color);
            float len = sqrtf((b[0] - a[0]) * (b[0] - a[0]) + (b[1] - a[1]) * (b[1] - a[1]));
            phase = fmodf(phase + len, dash + gap);
        }
        else
        {
            float d[5] = {a[0], a[1], b[0], b[1], t};
            prep_cmd_data(p, svg_cmd_line, d, 5, box, color);
        }
    }

    if (!dashed && n > 2)
    {
        for (uint32_t i = close ? 0 : 1; i < n; ++i)
        {
            const float* a = p->verts + i * 2;
            float d[3] = {a[0], a[1], t * .5f};
            float box[4] = {a[0] - t * .5f, a[1] - t * .5f, a[0] + t * .5f, a[1] + t * .5f};
            prep_cmd_data(p, svg_cmd_disc, d, 3, box, color);
        }
    }
    p->num_verts = 0;
}

static void prep_path_stroke(svg_prep* p, const svg_ctx* c, const svg_element* e, const svg_style* st, uint32_t color)
{
    const svg_path_cmd* cmds = p->doc->paths + e->path;
    float x = 0.f, y = 0.f, start_x = 0.f, start_y = 0.f;
    bool have_sub = false;

    for (uint32_t i = 0; i < e->num_path; ++i)
    {
        const svg_path_cmd* cmd = &cmds[i];
        switch (cmd->op)
        {
        case svg_path_move:
            if (have_sub)
                prep_stroke_run(p, c, st, color, false);
            p->num_verts = 0;
            p->oversized = false;
            x = start_x = cmd->v[0];
            y = start_y = cmd->v[1];
            prep_point(p, &c->m, x, y);
            have_sub = true;
            break;
        case svg_path_line:
            prep_point(p, &c->m, cmd->v[0], cmd->v[1]);
            x = cmd->v[0];
            y = cmd->v[1];
            break;
        case svg_path_hline:
            y = cmd->v[0];
            prep_point(p, &c->m, x, y);
            break;
        case svg_path_vline:
            x = cmd->v[0];
            prep_point(p, &c->m, x, y);
            break;
        case svg_path_cubic:
            prep_cubic(p, &c->m, x, y, cmd->v);
            x = cmd->v[4];
            y = cmd->v[5];
            break;
        case svg_path_quadratic:
            prep_quad(p, &c->m, x, y, cmd->v);
            x = cmd->v[2];
            y = cmd->v[3];
            break;
        case svg_path_arc:
            prep_arc(p, &c->m, x, y, cmd->v);
            x = cmd->v[5];
            y = cmd->v[6];
            break;
        case svg_path_close:
            if (have_sub)
                prep_stroke_run(p, c, st, color, true);
            x = start_x;
            y = start_y;
            have_sub = false;
            break;
        }
    }
    if (have_sub)
        prep_stroke_run(p, c, st, color, false);
    p->num_verts = 0;
}

static void prep_path_fill(svg_prep* p, const svg_ctx* c, const svg_element* e, uint32_t color)
{
    const svg_path_cmd* cmds = p->doc->paths + e->path;
    uint32_t n = e->num_path;
    float x = 0.f, y = 0.f;
    bool have_sub = false;

    for (uint32_t i = 0; i < n; ++i)
    {
        const svg_path_cmd* cmd = &cmds[i];
        switch (cmd->op)
        {
        case svg_path_move:
            if (have_sub)
                prep_emit_poly(p, color);
            x = cmd->v[0];
            y = cmd->v[1];
            prep_point(p, &c->m, x, y);
            have_sub = true;
            break;
        case svg_path_line:
            prep_point(p, &c->m, cmd->v[0], cmd->v[1]);
            x = cmd->v[0];
            y = cmd->v[1];
            have_sub = true;
            break;
        case svg_path_hline:
            y = cmd->v[0];
            prep_point(p, &c->m, x, y);
            have_sub = true;
            break;
        case svg_path_vline:
            x = cmd->v[0];
            prep_point(p, &c->m, x, y);
            have_sub = true;
            break;
        case svg_path_cubic:
            prep_cubic(p, &c->m, x, y, cmd->v);
            x = cmd->v[4];
            y = cmd->v[5];
            have_sub = true;
            break;
        case svg_path_quadratic:
            prep_quad(p, &c->m, x, y, cmd->v);
            x = cmd->v[2];
            y = cmd->v[3];
            have_sub = true;
            break;
        case svg_path_arc:
            prep_arc(p, &c->m, x, y, cmd->v);
            x = cmd->v[5];
            y = cmd->v[6];
            have_sub = true;
            break;
        case svg_path_close:
            if (have_sub)
                prep_emit_poly(p, color);
            have_sub = false;
            break;
        }
    }
    if (have_sub)
        prep_emit_poly(p, color);
}

static void prep_text_walk(svg_prep* p, const svg_ctx* c, const svg_element* e, const svg_style* st, float cx, float cy)
{
    if (e->text_len > 0)
        prep_draw_text(p, c, cx, cy, p->doc->text + e->text, e->text_len, st);

    for (uint32_t child = e->first_child; child != SVG_INVALID; child = p->doc->elements[child].next_sibling)
    {
        const svg_element* ts = &p->doc->elements[child];
        if (ts->type != svg_el_tspan)
            continue;
        svg_style ts_st;
        svg_resolve_style(p->doc, child, st, &ts_st);
        if (ts_st.display || ts_st.visibility)
            continue;
        uint32_t mask = (uint32_t)prep_param(p->doc, ts, 4);
        float tx = (mask & 1) ? prep_param(p->doc, ts, 0) : cx;
        float ty = (mask & 2) ? prep_param(p->doc, ts, 1) : cy;
        tx += prep_param(p->doc, ts, 2);
        ty += prep_param(p->doc, ts, 3);
        prep_text_walk(p, c, ts, &ts_st, tx, ty);
        cx = tx;
        cy = ty;
    }
}

static void prep_walk(svg_prep* p, svg_ctx* ctx, uint32_t idx, const svg_style* inherited)
{
    svg_document* doc = p->doc;
    const svg_element* e = &doc->elements[idx];
    if (e->type == svg_el_defs || (e->type == svg_el_symbol && !ctx->via_use))
        return;

    svg_style st;
    svg_resolve_style(doc, idx, inherited, &st);
    if (st.display)
        return;

    const float* t = doc->transforms + e->transform;
    svg_xform em = {t[0], t[1], t[2], t[3], t[4], t[5]};
    svg_ctx c = *ctx;
    c.m = svg_xform_mul(&ctx->m, &em);
    c.cls = svg_xform_classify(&c.m);
    c.uniform_scale = (c.cls <= 1) ? svg_xform_uniform_scale(&c.m) : 0.f;
    c.opacity = ctx->opacity * st.opacity;

    bool clipped = false;

    // nested svg / symbol : clip the content to its box, map the viewBox onto the box
    if ((e->type == svg_el_svg || e->type == svg_el_symbol) && !ctx->via_use)
    {
        float x = prep_param(doc, e, 0);
        float y = prep_param(doc, e, 1);
        float w = prep_param(doc, e, 2);
        float h = prep_param(doc, e, 3);
        float minx = prep_param(doc, e, 4);
        float miny = prep_param(doc, e, 5);
        float vbw = prep_param(doc, e, 6);
        float vbh = prep_param(doc, e, 7);
        if (w <= 0.f)
            w = (vbw > 0.f) ? vbw : 100.f;
        if (h <= 0.f)
            h = (vbh > 0.f) ? vbh : 100.f;

        if (c.cls == 0)
        {
            float d[4] = {x, y, x + w, y + h};
            prep_push_clip(p, svg_cmd_clip_rect, d, 4, d);
            clipped = true;
        }
        else
            p->doc->flags |= svg_flag_clip_unsupported;

        if (vbw > 0.f && vbh > 0.f)
        {
            float mode = prep_param(doc, e, 8);
            float ax = prep_param(doc, e, 9);
            float ay = prep_param(doc, e, 10);
            float sw, sh;
            if (mode == 2.f)
            {
                sw = w / vbw;
                sh = h / vbh;
            }
            else
            {
                float s = (mode == 1.f) ? fmaxf(w / vbw, h / vbh) : fminf(w / vbw, h / vbh);
                sw = s;
                sh = s;
            }
            float tx = w - sw * vbw;
            float ty = h - sh * vbh;
            if (ax == 1.f) tx *= .5f;
            else if (ax == 2.f) tx = 0.f;
            if (ay == 1.f) ty *= .5f;
            else if (ay == 2.f) ty = 0.f;
            svg_xform bm = {sw, 0.f, 0.f, sh, x + tx - sw * minx, y + ty - sh * miny};
            c.m = svg_xform_mul(&c.m, &bm);
            c.cls = svg_xform_classify(&c.m);
            c.uniform_scale = (c.cls <= 1) ? svg_xform_uniform_scale(&c.m) : 0.f;
        }
    }

    // clip-path
    if (e->clip_len > 0)
    {
        uint32_t shape = prep_clip_shape(doc, e);
        if (shape == SVG_INVALID)
            p->doc->flags |= svg_flag_clip_unsupported;
        else if (c.cls != 0)
            p->doc->flags |= svg_flag_clip_unsupported;
        else
        {
            const svg_element* s = &doc->elements[shape];
            if (s->type == svg_el_rect)
            {
                float crx = prep_param(doc, s, 4), cry = prep_param(doc, s, 5);
                float rx = (crx > 0.f && cry > 0.f) ? (crx + cry) * .5f : fmaxf(crx, cry);
                float x = prep_param(doc, s, 0);
                float y = prep_param(doc, s, 1);
                float w = prep_param(doc, s, 2);
                float h = prep_param(doc, s, 3);
                if (w > 0.f && h > 0.f)
                {
                    if (rx > 0.f)
                    {
                        float d[5] = {x, y, x + w, y + h, rx};
                        float box[4] = {x, y, x + w, y + h};
                        prep_push_clip(p, svg_cmd_clip_rect_rounded, d, 5, box);
                    }
                    else
                    {
                        float d[4] = {x, y, x + w, y + h};
                        prep_push_clip(p, svg_cmd_clip_rect, d, 4, d);
                    }
                    clipped = true;
                }
            }
            else
            {
                float cx = prep_param(doc, s, 0);
                float cy = prep_param(doc, s, 1);
                float r = prep_param(doc, s, 2);
                if (r > 0.f)
                {
                    float d[3] = {cx, cy, r};
                    float box[4] = {cx - r, cy - r, cx + r, cy + r};
                    prep_push_clip(p, svg_cmd_clip_disc, d, 3, box);
                    clipped = true;
                }
            }
        }
    }

    // use : instance the referenced element
    if (e->type == svg_el_use)
    {
        uint32_t ref = prep_find_ref(doc, e);
        if (ref != SVG_INVALID)
        {
            bool loop = false;
            for (uint32_t i = 0; i < c.use_depth; ++i)
            {
                if (c.use_stack[i] == ref)
                    loop = true;
            }
            if (!loop && c.use_depth < 64)
            {
                float ux = prep_param(doc, e, 0);
                float uy = prep_param(doc, e, 1);
                const svg_element* r = &doc->elements[ref];

                if (r->type == svg_el_symbol || r->type == svg_el_svg)
                {
                    float minx = prep_param(doc, r, 4);
                    float miny = prep_param(doc, r, 5);
                    float vbw = prep_param(doc, r, 6);
                    float vbh = prep_param(doc, r, 7);
                    float bw = (e->param_mask & (1u << 2) && prep_param(doc, e, 2) > 0.f) ? prep_param(doc, e, 2)
                                                                       : (vbw > 0.f ? vbw : 100.f);
                    float bh = (e->param_mask & (1u << 3) && prep_param(doc, e, 3) > 0.f) ? prep_param(doc, e, 3)
                                                                       : (vbh > 0.f ? vbh : 100.f);

                    if (vbw > 0.f && vbh > 0.f)
                    {
                        float mode = prep_param(doc, r, 8);
                        float ax = prep_param(doc, r, 9);
                        float ay = prep_param(doc, r, 10);
                        float sw, sh;
                        if (mode == 2.f)
                        {
                            sw = bw / vbw;
                            sh = bh / vbh;
                        }
                        else
                        {
                            float s = (mode == 1.f) ? fmaxf(bw / vbw, bh / vbh) : fminf(bw / vbw, bh / vbh);
                            sw = s;
                            sh = s;
                        }
                        float tx = bw - sw * vbw;
                        float ty = bh - sh * vbh;
                        if (ax == 1.f) tx *= .5f;
                        else if (ax == 2.f) tx = 0.f;
                        if (ay == 1.f) ty *= .5f;
                        else if (ay == 2.f) ty = 0.f;
                        svg_xform bm = {sw, 0.f, 0.f, sh, ux + tx - sw * minx, uy + ty - sh * miny};
                        c.m = svg_xform_mul(&c.m, &bm);
                    }
                    else
                    {
                        svg_xform tr = {1.f, 0.f, 0.f, 1.f, ux, uy};
                        c.m = svg_xform_mul(&c.m, &tr);
                    }
                    c.cls = svg_xform_classify(&c.m);
                    c.uniform_scale = (c.cls <= 1) ? svg_xform_uniform_scale(&c.m) : 0.f;

                    if (c.cls == 0)
                    {
                        float d[4] = {ux, uy, ux + bw, uy + bh};
                        prep_push_clip(p, svg_cmd_clip_rect, d, 4, d);
                        clipped = true;
                    }
                    else
                        p->doc->flags |= svg_flag_clip_unsupported;
                }
                else
                {
                    svg_xform tr = {1.f, 0.f, 0.f, 1.f, ux, uy};
                    c.m = svg_xform_mul(&c.m, &tr);
                    c.cls = svg_xform_classify(&c.m);
                    c.uniform_scale = (c.cls <= 1) ? svg_xform_uniform_scale(&c.m) : 0.f;
                }

                c.via_use = true;
                c.use_stack[c.use_depth++] = ref;
                prep_walk(p, &c, ref, &st);
            }
        }
        if (clipped)
            prep_push_clip(p, svg_cmd_clip_pop, NULL, 0, NULL);
        return;
    }

    if (!st.visibility)
    {
        uint32_t fill_color = prep_color(&st, &c, true);
        uint32_t stroke_color = prep_color(&st, &c, false);
        bool dashed = st.dash_set && st.num_dash > 0;
        float dash = st.dash[0];
        float gap = (st.num_dash > 1) ? st.dash[1] : dash;

        switch (e->type)
        {
        case svg_el_rect:
        {
            float x = prep_param(doc, e, 0);
            float y = prep_param(doc, e, 1);
            float w = prep_param(doc, e, 2);
            float h = prep_param(doc, e, 3);
            float rx = prep_param(doc, e, 4);
            float ry = prep_param(doc, e, 5);
            prep_draw_rect(p, &c, x, y, w, h, rx, ry, !st.fill_none, fill_color, stroke_color,
                           st.stroke_none ? 0.f : st.stroke_width, dashed, dash, gap, st.dash_offset, st.linecap == 1);
            break;
        }
        case svg_el_circle:
        {
            float cx = prep_param(doc, e, 0);
            float cy = prep_param(doc, e, 1);
            float r = prep_param(doc, e, 2);
            prep_draw_circle(p, &c, cx, cy, r, !st.fill_none, fill_color, stroke_color,
                             st.stroke_none ? 0.f : st.stroke_width, dashed, dash, gap, st.dash_offset, st.linecap == 1);
            break;
        }
        case svg_el_ellipse:
        {
            float cx = prep_param(doc, e, 0);
            float cy = prep_param(doc, e, 1);
            float rx = prep_param(doc, e, 2);
            float ry = prep_param(doc, e, 3);
            if (ry <= 0.f)
                ry = rx;
            prep_draw_ellipse(p, &c, cx, cy, rx, ry, !st.fill_none, fill_color, stroke_color,
                              st.stroke_none ? 0.f : st.stroke_width, dashed, dash, gap, st.dash_offset,
                              st.linecap == 1);
            break;
        }
        case svg_el_line:
        {
            if (!st.stroke_none)
                prep_draw_line(p, &c, prep_param(doc, e, 0), prep_param(doc, e, 1), prep_param(doc, e, 2),
                               prep_param(doc, e, 3), st.stroke_width, st.linecap, dashed, dash, gap, st.dash_offset,
                               stroke_color);
            break;
        }
        case svg_el_polygon:
        case svg_el_polyline:
        {
            if (!st.fill_none && e->num_points >= 6)
            {
                const float* pts = doc->points + e->points;
                for (uint32_t i = 0; i < e->num_points; i += 2)
                    prep_point(p, &c.m, pts[i], pts[i + 1]);
                prep_emit_poly(p, fill_color);
            }
            if (!st.stroke_none && st.stroke_width > 0.f && e->num_points >= 4)
            {
                const float* pts = doc->points + e->points;
                for (uint32_t i = 0; i < e->num_points; i += 2)
                    prep_point(p, &c.m, pts[i], pts[i + 1]);
                prep_stroke_run(p, &c, &st, stroke_color, e->type == svg_el_polygon);
            }
            break;
        }
        case svg_el_path:
        {
            if (e->path != SVG_INVALID)
            {
                if (!st.fill_none)
                    prep_path_fill(p, &c, e, fill_color);
                if (!st.stroke_none && st.stroke_width > 0.f)
                    prep_path_stroke(p, &c, e, &st, stroke_color);
            }
            break;
        }
        case svg_el_text:
        {
            if (!st.fill_none)
                prep_text_walk(p, &c, e, &st, prep_param(doc, e, 0), prep_param(doc, e, 1));
            break;
        }
        case svg_el_image:
            p->doc->flags |= svg_flag_image;
            break;
        default:
            break;
        }
    }

    for (uint32_t child = e->first_child; child != SVG_INVALID; child = doc->elements[child].next_sibling)
        prep_walk(p, &c, child, &st);

    if (clipped)
        prep_push_clip(p, svg_cmd_clip_pop, NULL, 0, NULL);
}

//----------------------------------------------------------------------------------------------------------------------------
// public api

svg_draw_list* svg_prepare(svg_document* doc, float scale, float tolerance, svg_allocator allocator)
{
    if (doc == NULL || scale <= 0.f || !isfinite(scale))
        return NULL;
    if (tolerance <= 0.f || !isfinite(tolerance))
        tolerance = 0.25f;

    svg_prep p;
    memset(&p, 0, sizeof(p));
    p.doc = doc;
    p.tolerance = tolerance;
    p.count = true;

    svg_ctx root;
    memset(&root, 0, sizeof(root));
    root.m = svg_xform_identity();
    root.m.a = scale;
    root.m.d = scale;
    root.cls = (scale == 1.f) ? 0 : 1;
    root.uniform_scale = scale;
    root.opacity = 1.f;

    prep_walk(&p, &root, doc->root, NULL);
    if (p.error)
        return NULL;

    uint32_t total_commands = p.num_commands;
    uint32_t total_data = p.num_data;
    uint32_t total_strings = p.num_strings;

    svg_draw_list* list = (svg_draw_list*)svg_mem_alloc(&(svg_mem){allocator}, sizeof(svg_draw_list), sizeof(void*));
    if (list == NULL)
        return NULL;
    memset(list, 0, sizeof(*list));
    list->alloc = allocator;

    bool ok = true;
    if (total_commands > 0)
        ok = (list->cmds = (svg_cmd*)svg_mem_alloc(&(svg_mem){allocator}, (size_t)total_commands * sizeof(svg_cmd),
                                                   sizeof(void*))) != NULL;
    if (ok && total_data > 0)
        ok = (list->data = (float*)svg_mem_alloc(&(svg_mem){allocator}, (size_t)total_data * sizeof(float),
                                                 sizeof(float))) != NULL;
    if (ok && total_strings > 0)
        ok = (list->strings = (char*)svg_mem_alloc(&(svg_mem){allocator}, (size_t)total_strings, 1)) != NULL;
    if (!ok)
    {
        svg_free_draw_list(list, allocator);
        return NULL;
    }

    p.count = false;
    p.list = list;
    p.data_off = 0;
    p.strings_off = 0;
    prep_walk(&p, &root, doc->root, NULL);
    // the write pass uses list->num_commands and the p cursors as write positions
    if (p.error || list->num_commands != total_commands || p.data_off != total_data || p.strings_off != total_strings)
    {
        svg_free_draw_list(list, allocator);
        return NULL;
    }
    list->num_data = total_data;
    list->num_strings = total_strings;
    return list;
}

void svg_free_draw_list(svg_draw_list* list, svg_allocator allocator)
{
    if (list == NULL)
        return;
    svg_mem m = {allocator};
    svg_mem_free(&m, list->cmds);
    svg_mem_free(&m, list->data);
    svg_mem_free(&m, list->strings);
    svg_mem_free(&m, list);
}

void svg_report(const svg_draw_list* list, uint32_t* num_commands, uint32_t* num_data_floats)
{
    if (num_commands)
        *num_commands = list ? list->num_commands : 0;
    if (num_data_floats)
        *num_data_floats = list ? list->num_data : 0;
}

void svg_draw(struct onedraw* r, svg_draw_list* list, float x, float y, uint32_t viewport_width, uint32_t viewport_height)
{
    if (list == NULL || list->num_commands == 0)
        return;

    float clip[4] = {x, y, x + (float)viewport_width, y + (float)viewport_height};
    float saved[32][4];
    uint32_t local_depth = 0;
    uint32_t emit_depth = 0;
    bool empty = false;

    for (uint32_t i = 0; i < list->num_commands; ++i)
    {
        const svg_cmd* c = &list->cmds[i];
        float b[4] = {c->box[0] + x, c->box[1] + y, c->box[2] + x, c->box[3] + y};

        if (c->kind == svg_cmd_clip_pop)
        {
            if (local_depth > 0)
            {
                --local_depth;
                if (local_depth < 32)
                    memcpy(clip, saved[local_depth], sizeof(clip));
                if (emit_depth > 0)
                {
                    --emit_depth;
                    od_pop_clip(r);
                }
                empty = clip[0] >= clip[2] || clip[1] >= clip[3];
            }
            continue;
        }

        if (c->kind == svg_cmd_clip_rect || c->kind == svg_cmd_clip_rect_rounded || c->kind == svg_cmd_clip_disc)
        {
            const float* d = list->data + c->data_off;
            float minx, miny, maxx, maxy;
            if (c->kind == svg_cmd_clip_disc)
            {
                minx = d[0] - d[2] + x;
                miny = d[1] - d[2] + y;
                maxx = d[0] + d[2] + x;
                maxy = d[1] + d[2] + y;
            }
            else
            {
                minx = b[0];
                miny = b[1];
                maxx = b[2];
                maxy = b[3];
            }
            if (local_depth < 32)
                memcpy(saved[local_depth], clip, sizeof(clip));
            if (minx > clip[0]) clip[0] = minx;
            if (miny > clip[1]) clip[1] = miny;
            if (maxx < clip[2]) clip[2] = maxx;
            if (maxy < clip[3]) clip[3] = maxy;
            ++local_depth;
            if (!(clip[0] >= clip[2] || clip[1] >= clip[3]) && emit_depth < 16)
            {
                if (c->kind == svg_cmd_clip_disc)
                    od_canvas_clipdisc(r, d[0] + x, d[1] + y, d[2]);
                else if (c->kind == svg_cmd_clip_rect_rounded)
                    od_canvas_cliprect_rounded(r, d[0] + x, d[1] + y, d[2] + x, d[3] + y, d[4]);
                else
                    od_canvas_cliprect(r, d[0] + x, d[1] + y, d[2] + x, d[3] + y);
                ++emit_depth;
            }
            empty = clip[0] >= clip[2] || clip[1] >= clip[3];
            continue;
        }

        if (empty)
            continue;

        if (b[2] < clip[0] || b[0] > clip[2] || b[3] < clip[1] || b[1] > clip[3])
            continue;
        if (b[2] < x || b[0] > x + (float)viewport_width || b[3] < y || b[1] > y + (float)viewport_height)
            continue;

        const float* d = list->data + c->data_off;
        switch (c->kind)
        {
        case svg_cmd_polygon:
        {
            uint32_t nv = (uint32_t)d[0];
            float tmp[OD_MAX_POLYGON_VERTICES * 2];
            for (uint32_t k = 0; k < nv * 2; k += 2)
            {
                tmp[k] = d[1 + k] + x;
                tmp[k + 1] = d[2 + k] + y;
            }
            od_draw_polygon(r, tmp, nv, 0.f, c->color);
            break;
        }
        case svg_cmd_line:
            od_draw_line(r, d[0] + x, d[1] + y, d[2] + x, d[3] + y, d[4], c->color);
            break;
        case svg_cmd_capsule:
            od_draw_capsule(r, d[0] + x, d[1] + y, d[2] + x, d[3] + y, d[4], c->color);
            break;
        case svg_cmd_box:
            od_draw_box(r, d[0] + x, d[1] + y, d[2] + x, d[3] + y, d[4], c->color);
            break;
        case svg_cmd_oriented_box:
            od_draw_oriented_box(r, d[0] + x, d[1] + y, d[2] + x, d[3] + y, d[4], d[5], c->color);
            break;
        case svg_cmd_oriented_rect:
            od_draw_oriented_rect(r, d[0] + x, d[1] + y, d[2] + x, d[3] + y, d[4], d[5], d[6], c->color);
            break;
        case svg_cmd_disc:
            od_draw_disc(r, d[0] + x, d[1] + y, d[2], c->color);
            break;
        case svg_cmd_ring:
            od_draw_ring(r, d[0] + x, d[1] + y, d[2], d[3], c->color);
            break;
        case svg_cmd_ellipse:
            od_draw_ellipse(r, d[0] + x, d[1] + y, d[2] + x, d[3] + y, d[4], c->color);
            break;
        case svg_cmd_ellipse_ring:
            od_draw_ellipse_ring(r, d[0] + x, d[1] + y, d[2] + x, d[3] + y, d[4], d[5], c->color);
            break;
        case svg_cmd_dashed_line:
        case svg_cmd_dashed_ring:
        case svg_cmd_dashed_box:
        case svg_cmd_dashed_oriented_box:
        case svg_cmd_dashed_ellipse_ring:
        {
            od_dash_style style;
            uint32_t base;
            if (c->kind == svg_cmd_dashed_line)
                base = 5;
            else if (c->kind == svg_cmd_dashed_ring)
                base = 4;
            else if (c->kind == svg_cmd_dashed_box)
                base = 6;
            else if (c->kind == svg_cmd_dashed_oriented_box)
                base = 7;
            else
                base = 6;
            style.dash_length = d[base];
            style.gap_length = d[base + 1];
            style.offset = d[base + 2];
            style.round_caps = d[base + 3] != 0.f;
            if (c->kind == svg_cmd_dashed_line)
                od_draw_dashed_line(r, d[0] + x, d[1] + y, d[2] + x, d[3] + y, d[4], style, c->color);
            else if (c->kind == svg_cmd_dashed_ring)
                od_draw_dashed_ring(r, d[0] + x, d[1] + y, d[2], d[3], style, c->color);
            else if (c->kind == svg_cmd_dashed_box)
                od_draw_dashed_box(r, d[0] + x, d[1] + y, d[2] + x, d[3] + y, d[4], d[5], style, c->color);
            else if (c->kind == svg_cmd_dashed_oriented_box)
                od_draw_dashed_oriented_box(r, d[0] + x, d[1] + y, d[2] + x, d[3] + y, d[4], d[5], d[6], style,
                                            c->color);
            else
                od_draw_dashed_ellipse_ring(r, d[0] + x, d[1] + y, d[2] + x, d[3] + y, d[4], d[5], style, c->color);
            break;
        }
        case svg_cmd_text:
        {
            od_font font = (od_font)(int)d[3];
            float size = d[2];
            if (size <= 0.f)
                break;
            const char* s = list->strings + c->str_off;
            float w = od_string_view_width(r, s, c->str_len, font, size);
            float px = d[0] + x;
            if (d[4] == 1.f)
                px -= w * .5f;
            else if (d[4] == 2.f)
                px -= w;
            float py = d[1] + y - od_text_ascent(r, font, size);
            od_draw_string_view(r, px, py, s, c->str_len, font, size, c->color);
            break;
        }
        default:
            break;
        }
    }
}
