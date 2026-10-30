#include "mcp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SJ_IMPL
#include "../../third_party/sj.h"

#define SV_IMPLEMENTATION
#include "../../third_party/sv.h"

#define count_of(a)   (sizeof(a) / sizeof((a)[0]))

#define MCP_MAX_MEMBERS   (24)

//----------------------------------------------------------------------------------------------------------------------------
// #rrggbb or #rrggbbaa -> draw_color (A8R8G8B8)
//----------------------------------------------------------------------------------------------------------------------------
static bool hex_nibble(char c, uint32_t* v)
{
    if (c >= '0' && c <= '9')
        *v = (uint32_t) (c - '0');
    else if (c >= 'a' && c <= 'f')
        *v = (uint32_t) (c - 'a' + 10);
    else if (c >= 'A' && c <= 'F')
        *v = (uint32_t) (c - 'A' + 10);
    else
        return false;
    return true;
}

static bool hex_color(const char* data, size_t len, draw_color* out)
{
    if (data == NULL || (len != 7 && len != 9) || data[0] != '#')
        return false;

    uint32_t v = 0;
    for (size_t i = 1; i < len; i++)
    {
        uint32_t n;
        if (!hex_nibble(data[i], &n))
            return false;
        v = (v << 4) | n;
    }

    if (len == 7)
    {
        // rrggbb : fully opaque
        *out = v | 0xff000000;
    }
    else
    {
        // rrggbbaa -> A8R8G8B8 : v is 0xRRGGBBAA, take A to the top and rotate RRGGBB down
        *out = ((v & 0xff) << 24) | ((v & 0xffffff00) >> 8);
    }
    return true;
}

bool mcp_parse_color(const char* str, draw_color* out)
{
    if (str == NULL)
        return false;
    return hex_color(str, strlen(str), out);
}

//----------------------------------------------------------------------------------------------------------------------------
// sj parsing context
//
// sj cursors only walk forward, so a container (object/array) must be consumed IN PLACE, the
// moment it is read : re-visiting it afterwards would read the following content. Leaf values
// (numbers, strings, booleans) are safe to keep as pointers into the buffer and look up later.
//
// The parse of one object is therefore a single pass that :
//   - stores leaf members (for the gnum/gstr/gbool/gcolor lookups)
//   - consumes the 'dash' object in place (parse_dash)
//   - consumes the 'primitives' array in place, parsing each element (a draw)
//   - drains any other unexpected container to keep the cursor coherent
//----------------------------------------------------------------------------------------------------------------------------
typedef struct mcp_member
{
    const char* key;
    size_t key_len;
    sj_Value val;
} mcp_member;

typedef struct mcp_parse
{
    sj_Reader* r;
    mcp_scene* s;
    char* err;
    size_t err_size;
    sj_Value ctx;             // the object being parsed (error positions)
    mcp_member m[MCP_MAX_MEMBERS];
    int n;
    od_dash_style dash;
    bool has_dash;
} mcp_parse;

static void perr_plain(char* err, size_t n, const char* msg)
{
    snprintf(err, n, "%s", msg);
}

static void perr(mcp_parse* p, const char* msg)
{
    int line = 0, col = 0;
    sj_location(p->r, &line, &col);
    snprintf(p->err, p->err_size, "%s (line %d, col %d)", msg, line, col);
}

// Line / col of the first character of [v] (mirrors sj_location)
static void value_location(sj_Reader* r, sj_Value v, int* line, int* col)
{
    int ln = 1, cl = 1;
    for (char* p = r->data; p < v.start; p++)
    {
        if (*p == '\n')
        {
            ln++;
            cl = 0;
        }
        cl++;
    }
    *line = ln;
    *col = cl;
}

static bool key_eq(sj_Value k, const char* key)
{
    size_t key_len = strlen(key);
    return k.type == SJ_STRING && (size_t) (k.end - k.start) == key_len && memcmp(k.start, key, key_len) == 0;
}

static bool add_member(mcp_parse* p, sj_Value k, sj_Value v)
{
    if (p->n >= MCP_MAX_MEMBERS)
    {
        perr(p, "too many fields in object");
        return false;
    }
    p->m[p->n] = (mcp_member) { .key = k.start, .key_len = (size_t) (k.end - k.start), .val = v };
    p->n++;
    return true;
}

// Reads until the cursor is back out of the container [v] (which starts at [v].depth)
static bool drain_container(mcp_parse* p, sj_Value v)
{
    while (p->r->depth > v.depth)
    {
        sj_Value d = sj_read(p->r);
        if (d.type == SJ_ERROR)
            return false;
    }
    return true;
}

static bool check_reader(mcp_parse* p)
{
    if (p->r->error == NULL)
        return true;
    perr(p, p->r->error);
    return false;
}

static const mcp_member* find_member(const mcp_parse* p, const char* key)
{
    size_t key_len = strlen(key);
    for (int i = 0; i < p->n; i++)
        if (p->m[i].key_len == key_len && memcmp(p->m[i].key, key, key_len) == 0)
            return &p->m[i];
    return NULL;
}

static bool sv_to_float(const sj_Value* v, float* out)
{
    if (v->start == v->end)
        return false;

    char tmp[64];
    size_t len = (size_t) (v->end - v->start);
    if (len >= sizeof(tmp))
        return false;

    memcpy(tmp, v->start, len);
    tmp[len] = '\0';

    char* end = NULL;
    double d = strtod(tmp, &end);
    if (end == tmp || *end != '\0')
        return false;

    *out = (float) d;
    return true;
}

// A number field : [out] is left untouched when absent (the caller holds the default),
// error when present but not a valid number
static bool gnum(const mcp_parse* p, const char* key, float* out, bool required)
{
    const mcp_member* mem = find_member(p, key);
    if (mem == NULL)
    {
        if (required)
        {
            int line = 0, col = 0;
            value_location(p->r, p->ctx, &line, &col);
            snprintf(p->err, p->err_size, "missing field '%s' (line %d, col %d)", key, line, col);
            return false;
        }
        return true;
    }
    if (mem->val.type != SJ_NUMBER)
    {
        int line = 0, col = 0;
        value_location(p->r, mem->val, &line, &col);
        snprintf(p->err, p->err_size, "field '%s' must be a number (line %d, col %d)", key, line, col);
        return false;
    }

    return sv_to_float(&mem->val, out);
}

// A string field : false when absent (or not a string), [data]/[len] otherwise (no NUL added)
static bool gstr(const mcp_parse* p, const char* key, const char** data, size_t* len, bool required)
{
    const mcp_member* mem = find_member(p, key);
    if (mem == NULL)
    {
        if (required)
        {
            int line = 0, col = 0;
            value_location(p->r, p->ctx, &line, &col);
            snprintf(p->err, p->err_size, "missing field '%s' (line %d, col %d)", key, line, col);
            return false;
        }
        return false;
    }
    if (mem->val.type != SJ_STRING)
    {
        int line = 0, col = 0;
        value_location(p->r, mem->val, &line, &col);
        snprintf(p->err, p->err_size, "field '%s' must be a string (line %d, col %d)", key, line, col);
        return false;
    }

    *data = mem->val.start;
    *len = (size_t) (mem->val.end - mem->val.start);
    return true;
}

static bool gbool(const mcp_parse* p, const char* key, bool* out)
{
    *out = false;
    const mcp_member* mem = find_member(p, key);
    if (mem == NULL)
        return true;
    if (mem->val.type != SJ_BOOL)
    {
        int line = 0, col = 0;
        value_location(p->r, mem->val, &line, &col);
        snprintf(p->err, p->err_size, "field '%s' must be a boolean (line %d, col %d)", key, line, col);
        return false;
    }

    *out = mem->val.start[0] == 't';
    return true;
}

// A #rrggbb[a] color field : [out] is left untouched when absent
static bool gcolor(const mcp_parse* p, const char* key, draw_color* out)
{
    const char* data = NULL;
    size_t len = 0;
    if (!gstr(p, key, &data, &len, false))
        return true;

    if (!hex_color(data, len, out))
    {
        const mcp_member* mem = find_member(p, key);
        int line = 0, col = 0;
        value_location(p->r, mem->val, &line, &col);
        snprintf(p->err, p->err_size, "field '%s' must be #rrggbb or #rrggbbaa (line %d, col %d)", key, line, col);
        return false;
    }
    return true;
}

//----------------------------------------------------------------------------------------------------------------------------
// Primitives (plan §6)
//----------------------------------------------------------------------------------------------------------------------------
typedef struct mcp_prim_def
{
    const char* label;
    enum mcp_op_type type;
    int nkeys;
    const char* keys[9];
    bool dash;
} mcp_prim_def;

// [keys] match the [f] slots documented in mcp.h, in order
static const mcp_prim_def prim_defs[] = {
    { .label = "disc",                  .type = MCP_OP_DISC,                  .nkeys = 3, .keys = { "cx", "cy", "radius" },                        .dash = false },
    { .label = "ring",                  .type = MCP_OP_RING,                  .nkeys = 4, .keys = { "cx", "cy", "radius", "thickness" },          .dash = false },
    { .label = "disc_gradient",         .type = MCP_OP_DISC_GRADIENT,         .nkeys = 3, .keys = { "cx", "cy", "radius" },                        .dash = false },
    { .label = "box",                   .type = MCP_OP_BOX,                   .nkeys = 5, .keys = { "x0", "y0", "x1", "y1", "radius" },           .dash = false },
    { .label = "blurred_box",           .type = MCP_OP_BLURRED_BOX,           .nkeys = 5, .keys = { "cx", "cy", "half_width", "half_height", "roundness" }, .dash = false },
    { .label = "line",                  .type = MCP_OP_LINE,                  .nkeys = 5, .keys = { "ax", "ay", "bx", "by", "width" },            .dash = false },
    { .label = "capsule",               .type = MCP_OP_CAPSULE,               .nkeys = 5, .keys = { "ax", "ay", "bx", "by", "radius" },           .dash = false },
    { .label = "capsule_gradient",      .type = MCP_OP_CAPSULE_GRADIENT,      .nkeys = 5, .keys = { "ax", "ay", "bx", "by", "radius" },           .dash = false },
    { .label = "oriented_box",          .type = MCP_OP_ORIENTED_BOX,          .nkeys = 6, .keys = { "ax", "ay", "bx", "by", "width", "roundness" }, .dash = false },
    { .label = "oriented_rect",         .type = MCP_OP_ORIENTED_RECT,         .nkeys = 7, .keys = { "ax", "ay", "bx", "by", "width", "roundness", "thickness" }, .dash = false },
    { .label = "ellipse",               .type = MCP_OP_ELLIPSE,               .nkeys = 5, .keys = { "ax", "ay", "bx", "by", "width" },            .dash = false },
    { .label = "ellipse_ring",          .type = MCP_OP_ELLIPSE_RING,          .nkeys = 6, .keys = { "ax", "ay", "bx", "by", "width", "thickness" }, .dash = false },
    { .label = "triangle",              .type = MCP_OP_TRIANGLE,              .nkeys = 7, .keys = { "ax", "ay", "bx", "by", "cx", "cy", "roundness" }, .dash = false },
    { .label = "triangle_ring",         .type = MCP_OP_TRIANGLE_RING,         .nkeys = 8, .keys = { "ax", "ay", "bx", "by", "cx", "cy", "roundness", "thickness" }, .dash = false },
    { .label = "sector",                .type = MCP_OP_SECTOR,                .nkeys = 5, .keys = { "cx", "cy", "radius", "start_angle", "sweep_angle" }, .dash = false },
    { .label = "sector_ring",           .type = MCP_OP_SECTOR_RING,           .nkeys = 6, .keys = { "cx", "cy", "radius", "start_angle", "sweep_angle", "thickness" }, .dash = false },
    { .label = "arc",                   .type = MCP_OP_ARC,                   .nkeys = 7, .keys = { "cx", "cy", "dx", "dy", "aperture", "radius", "thickness" }, .dash = false },
    { .label = "quadratic_bezier",      .type = MCP_OP_QUADRATIC_BEZIER,      .nkeys = 7, .keys = { "ax", "ay", "cx", "cy", "bx", "by", "width" },  .dash = false },
    { .label = "cubic_bezier",          .type = MCP_OP_CUBIC_BEZIER,          .nkeys = 9, .keys = { "ax", "ay", "c1x", "c1y", "c2x", "c2y", "bx", "by", "width" }, .dash = false },
    { .label = "char",                  .type = MCP_OP_CHAR,                  .nkeys = 2, .keys = { "x", "y" },                                  .dash = false },
    { .label = "text",                  .type = MCP_OP_TEXT,                  .nkeys = 2, .keys = { "x", "y" },                                  .dash = false },
    { .label = "dashed_line",           .type = MCP_OP_DASHED_LINE,           .nkeys = 5, .keys = { "ax", "ay", "bx", "by", "width" },            .dash = true },
    { .label = "dashed_ring",           .type = MCP_OP_DASHED_RING,           .nkeys = 4, .keys = { "cx", "cy", "radius", "thickness" },          .dash = true },
    { .label = "dashed_box",            .type = MCP_OP_DASHED_BOX,            .nkeys = 6, .keys = { "x0", "y0", "x1", "y1", "radius", "thickness" }, .dash = true },
    { .label = "dashed_oriented_box",   .type = MCP_OP_DASHED_ORIENTED_BOX,   .nkeys = 7, .keys = { "ax", "ay", "bx", "by", "width", "roundness", "thickness" }, .dash = true },
    { .label = "dashed_ellipse_ring",   .type = MCP_OP_DASHED_ELLIPSE_RING,   .nkeys = 6, .keys = { "ax", "ay", "bx", "by", "width", "thickness" }, .dash = true },
    { .label = "dashed_triangle_ring",  .type = MCP_OP_DASHED_TRIANGLE_RING,  .nkeys = 7, .keys = { "ax", "ay", "bx", "by", "cx", "cy", "thickness" }, .dash = true },
    { .label = "begin_group",           .type = MCP_OP_BEGIN_GROUP,           .nkeys = 2, .keys = { "smooth_value", "outline_width" },             .dash = false },
    { .label = "end_group",             .type = MCP_OP_END_GROUP,             .nkeys = 0, .keys = { 0 },                                       .dash = false },
    { .label = "push_cliprect",         .type = MCP_OP_PUSH_CLIPRECT,         .nkeys = 4, .keys = { "min_x", "min_y", "max_x", "max_y" },         .dash = false },
    { .label = "push_cliprect_rounded", .type = MCP_OP_PUSH_CLIPRECT_ROUNDED, .nkeys = 5, .keys = { "min_x", "min_y", "max_x", "max_y", "radius" }, .dash = false },
    { .label = "push_clipdisc",         .type = MCP_OP_PUSH_CLIPDISC,         .nkeys = 3, .keys = { "cx", "cy", "radius" },                        .dash = false },
    { .label = "pop_clip",              .type = MCP_OP_POP_CLIP,              .nkeys = 0, .keys = { 0 },                                       .dash = false },
};

// Parses the 'dash' object in place : {length, gap, offset, round_caps}
static bool parse_dash(mcp_parse* p, sj_Value dv)
{
    mcp_parse dp = *p;
    dp.ctx = dv;
    dp.n = 0;

    sj_Value k, v;
    while (sj_iter_object(p->r, dv, &k, &v))
    {
        if (k.type != SJ_STRING)
        {
            perr(p, "object keys must be strings");
            return false;
        }
        if (v.type == SJ_OBJECT || v.type == SJ_ARRAY)
        {
            if (!drain_container(p, v))
                return false;
            continue;
        }
        if (!add_member(&dp, k, v))
            return false;
    }
    if (!check_reader(p))
        return false;

    od_dash_style d = { 0 };
    if (!gnum(&dp, "length", &d.dash_length, true))
        return false;
    if (!gnum(&dp, "gap", &d.gap_length, false))
        return false;
    if (!gnum(&dp, "offset", &d.offset, false))
        return false;
    if (!gbool(&dp, "round_caps", &d.round_caps))
        return false;

    p->dash = d;
    p->has_dash = true;
    return true;
}

// Single pass over the primitive object [obj] : stores the leaf members, consumes the 'dash'
// object in place
static bool collect_primitive_leaves(mcp_parse* p, sj_Value obj)
{
    p->ctx = obj;
    p->n = 0;
    p->has_dash = false;

    sj_Value k, v;
    while (sj_iter_object(p->r, obj, &k, &v))
    {
        if (k.type != SJ_STRING)
        {
            perr(p, "object keys must be strings");
            return false;
        }
        if (v.type == SJ_OBJECT)
        {
            if (key_eq(k, "dash"))
            {
                if (!parse_dash(p, v))
                    return false;
            }
            else if (!drain_container(p, v))
            {
                return false;
            }
            continue;
        }
        if (v.type == SJ_ARRAY)
        {
            if (!drain_container(p, v))
                return false;
            continue;
        }
        if (!add_member(p, k, v))
            return false;
    }
    return check_reader(p);
}

// Builds the op from the stored members of [p] (the primitive fields)
static bool parse_primitive_fields(mcp_parse* p, mcp_op* out)
{
    memset(out, 0, sizeof(*out));
    out->color = 0xff000000;

    const char* type_data = NULL;
    size_t type_len = 0;
    if (!gstr(p, "type", &type_data, &type_len, true))
        return false;

    const mcp_prim_def* def = NULL;
    for (size_t i = 0; i < count_of(prim_defs); i++)
    {
        if (sv_eq(sv_from_parts(type_data, type_len), sv_from_cstr(prim_defs[i].label)))
        {
            def = &prim_defs[i];
            break;
        }
    }
    if (def == NULL)
    {
        char label[32];
        size_t len = type_len < 31 ? type_len : 31;
        memcpy(label, type_data, len);
        label[len] = '\0';
        snprintf(p->err, p->err_size, "unknown primitive type '%s'", label);
        return false;
    }

    out->type = def->type;
    for (int i = 0; i < def->nkeys; i++)
    {
        if (!gnum(p, def->keys[i], &out->f[i], false))
            return false;
    }

    if (!gcolor(p, "color", &out->color))
        return false;
    out->color2 = out->color;
    if (!gcolor(p, "color2", &out->color2))
        return false;

    if (def->type == MCP_OP_CHAR)
    {
        const char* ch = NULL;
        size_t ch_len = 0;
        if (!gstr(p, "c", &ch, &ch_len, true) || ch_len != 1)
        {
            snprintf(p->err, p->err_size, "the 'char' primitive needs a 1-character 'c' field");
            return false;
        }
        out->c = ch[0];
    }

    if (def->type == MCP_OP_TEXT)
    {
        const char* text = NULL;
        size_t text_len = 0;
        if (!gstr(p, "text", &text, &text_len, true))
            return false;

        out->text = mcp_scene_intern_len(p->s, text, text_len);
        if (out->text == NULL)
        {
            perr_plain(p->err, p->err_size, "out of memory");
            return false;
        }
    }

    if (def->type == MCP_OP_BEGIN_GROUP && !gbool(p, "smooth", &out->smoothblend))
        return false;

    if (def->dash)
        out->dash = p->has_dash ? p->dash : (od_dash_style) { 0 };

    return true;
}

//----------------------------------------------------------------------------------------------------------------------------
// Commands (plan §4)
//----------------------------------------------------------------------------------------------------------------------------
bool mcp_parse_tool_call(mcp_scene* s, char* buf, size_t len, mcp_parse_result* out, char* err, size_t err_size)
{
    memset(out, 0, sizeof(*out));
    err[0] = '\0';

    if (len == 0)
    {
        perr_plain(err, err_size, "empty input");
        return false;
    }
    buf[len] = '\0';

    sj_Reader r = sj_reader(buf, len);
    sj_Value v = sj_read(&r);
    if (v.type != SJ_OBJECT)
    {
        int line = 0, col = 0;
        sj_location(&r, &line, &col);
        snprintf(err, err_size, "expected a JSON object (line %d, col %d)", line, col);
        return false;
    }

    mcp_parse p = { .r = &r, .s = s, .err = err, .err_size = err_size, .ctx = v };

    // the parsed draw elements, held until the cmd is known (the scene is only mutated by draw)
    mcp_op* drawn = NULL;
    uint32_t drawn_count = 0;
    uint32_t drawn_cap = 0;
    sj_Value primitives = { .type = SJ_NULL };
    bool ok = false;

    sj_Value k, val;
    while (sj_iter_object(&r, v, &k, &val))
    {
        if (k.type != SJ_STRING)
        {
            perr(&p, "object keys must be strings");
            goto out;
        }

        if (key_eq(k, "primitives") && val.type == SJ_ARRAY)
        {
            // consume the array in place : each element is a primitive object
            primitives = val;
            sj_Value pv;
            mcp_op op;
            while (sj_iter_array(&r, val, &pv))
            {
                if (pv.type != SJ_OBJECT)
                {
                    perr(&p, "each primitive must be a JSON object");
                    goto out;
                }
                mcp_parse pp = p;
                if (!collect_primitive_leaves(&pp, pv))
                    goto out;
                if (!parse_primitive_fields(&pp, &op))
                    goto out;

                if (drawn_count == drawn_cap)
                {
                    uint32_t new_cap = drawn_cap ? drawn_cap * 2 : 16;
                    mcp_op* grown = (mcp_op*) realloc(drawn, (size_t) new_cap * sizeof(mcp_op));
                    if (grown == NULL)
                    {
                        perr_plain(err, err_size, "out of memory");
                        goto out;
                    }
                    drawn = grown;
                    drawn_cap = new_cap;
                }
                drawn[drawn_count++] = op;
            }
            if (!check_reader(&p))
                goto out;
            continue;
        }

        if (val.type == SJ_OBJECT)
        {
            if (key_eq(k, "dash"))
            {
                if (!parse_dash(&p, val))
                    goto out;
            }
            else if (!drain_container(&p, val))
            {
                goto out;
            }
            continue;
        }
        if (val.type == SJ_ARRAY)
        {
            if (!drain_container(&p, val))
                goto out;
            continue;
        }
        if (!add_member(&p, k, val))
            goto out;
    }
    if (!check_reader(&p))
        goto out;

    // the cmd
    {
        const char* cmd_data = NULL;
        size_t cmd_len = 0;
        if (!gstr(&p, "cmd", &cmd_data, &cmd_len, true))
            goto out;

        if (sv_eq(sv_from_parts(cmd_data, cmd_len), SV("clear")))
            out->cmd = MCP_CMD_CLEAR;
        else if (sv_eq(sv_from_parts(cmd_data, cmd_len), SV("draw")))
            out->cmd = MCP_CMD_DRAW;
        else if (sv_eq(sv_from_parts(cmd_data, cmd_len), SV("update")))
            out->cmd = MCP_CMD_UPDATE;
        else if (sv_eq(sv_from_parts(cmd_data, cmd_len), SV("remove")))
            out->cmd = MCP_CMD_REMOVE;
        else if (sv_eq(sv_from_parts(cmd_data, cmd_len), SV("screenshot")))
            out->cmd = MCP_CMD_SCREENSHOT;
        else
        {
            char label[32];
            size_t len2 = cmd_len < 31 ? cmd_len : 31;
            memcpy(label, cmd_data, len2);
            label[len2] = '\0';
            perr(&p, label);
            goto out;
        }
    }

    switch(out->cmd)
    {
    case MCP_CMD_CLEAR :
    {
        float w = 1024.f, h = 768.f;
        if (!gnum(&p, "width", &w, false) || !gnum(&p, "height", &h, false))
            goto out;
        if (w <= 0.f || h <= 0.f)
        {
            perr_plain(err, err_size, "width and height must be > 0");
            goto out;
        }

        draw_color background = 0xffebecdc;
        if (!gcolor(&p, "background", &background))
            goto out;

        mcp_scene_reset(s, w, h, background);
        out->live = 0;
        ok = true;
        break;
    }
    case MCP_CMD_DRAW :
    {
        if (primitives.type != SJ_ARRAY)
        {
            perr_plain(err, err_size, "missing 'primitives' array");
            goto out;
        }

        uint32_t first = UINT32_MAX;
        for (uint32_t i = 0; i < drawn_count; i++)
        {
            uint32_t index = mcp_scene_append(s, &drawn[i]);
            if (index == UINT32_MAX)
            {
                perr_plain(err, err_size, "out of memory");
                goto out;
            }
            if (first == UINT32_MAX)
                first = index;
        }
        out->index = first;
        out->count = drawn_count;
        out->live = mcp_scene_live_count(s);
        ok = true;
        break;
    }
    case MCP_CMD_UPDATE :
    {
        float idx = 0.f;
        if (!gnum(&p, "index", &idx, true) || idx < 0.f || idx > 4294967295.f)
        {
            perr_plain(err, err_size, "field 'index' must be a non-negative integer");
            goto out;
        }

        mcp_op op;
        if (!parse_primitive_fields(&p, &op))
            goto out;

        if (!mcp_scene_update(s, (uint32_t) idx, &op))
        {
            snprintf(err, err_size, "no live op at index %u", (uint32_t) idx);
            goto out;
        }

        out->index = (uint32_t) idx;
        out->live = mcp_scene_live_count(s);
        ok = true;
        break;
    }
    case MCP_CMD_REMOVE :
    {
        float idx = 0.f;
        if (!gnum(&p, "index", &idx, true) || idx < 0.f || idx > 4294967295.f)
        {
            perr_plain(err, err_size, "field 'index' must be a non-negative integer");
            goto out;
        }

        if (!mcp_scene_remove(s, (uint32_t) idx))
        {
            snprintf(err, err_size, "no live op at index %u", (uint32_t) idx);
            goto out;
        }

        out->index = (uint32_t) idx;
        out->live = mcp_scene_live_count(s);
        ok = true;
        break;
    }
    case MCP_CMD_SCREENSHOT :
    {
        const mcp_member* mw = find_member(&p, "width");
        const mcp_member* mh = find_member(&p, "height");
        if ((mw == NULL) != (mh == NULL))
        {
            perr_plain(err, err_size, "screenshot 'width' and 'height' must be given together");
            goto out;
        }
        if (mw != NULL)
        {
            if (mw->val.type != SJ_NUMBER || mh->val.type != SJ_NUMBER)
            {
                perr_plain(err, err_size, "screenshot 'width' and 'height' must be numbers");
                goto out;
            }

            float w = 0.f, h = 0.f;
            if (!sv_to_float(&mw->val, &w) || !sv_to_float(&mh->val, &h))
            {
                perr_plain(err, err_size, "screenshot 'width' and 'height' are not valid numbers");
                goto out;
            }
            if (w <= 0.f || h <= 0.f)
            {
                perr_plain(err, err_size, "screenshot width and height must be > 0");
                goto out;
            }
            if (w > 4096.f || h > 4096.f)
            {
                perr_plain(err, err_size, "screenshot size exceeds 4096x4096");
                goto out;
            }

            out->shot_w = (uint32_t) w;
            out->shot_h = (uint32_t) h;
        }
        ok = true;
        break;
    }
    default:
        break;
    }

out:
    free(drawn);
    return ok;
}
