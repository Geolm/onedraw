#include "mcp.h"

#include <stdlib.h>
#include <string.h>

#define MCP_SCENE_INITIAL_OPS    (256)
#define MCP_SCENE_INITIAL_TEXT   (4096)

//----------------------------------------------------------------------------------------------------------------------------
mcp_transform mcp_fit_transform(float canvas_w, float canvas_h, float fb_w, float fb_h)
{
    float scale = fb_w / canvas_w;
    float s2 = fb_h / canvas_h;
    if (s2 < scale)
        scale = s2;

    mcp_transform t;
    t.scale = scale;
    t.offx = (fb_w - canvas_w * scale) * 0.5f;
    t.offy = (fb_h - canvas_h * scale) * 0.5f;
    return t;
}

//----------------------------------------------------------------------------------------------------------------------------
void mcp_scene_init(mcp_scene* s)
{
    memset(s, 0, sizeof(*s));
    mcp_scene_reset(s, 1024.f, 768.f, 0xffebecdc);
}

//----------------------------------------------------------------------------------------------------------------------------
void mcp_scene_reset(mcp_scene* s, float canvas_w, float canvas_h, draw_color background)
{
    free(s->ops);
    free(s->text);
    memset(s, 0, sizeof(*s));
    s->canvas_w = canvas_w;
    s->canvas_h = canvas_h;
    s->background = background;
}

//----------------------------------------------------------------------------------------------------------------------------
static bool mcp_scene_grow_ops(mcp_scene* s, uint32_t min_capacity)
{
    uint32_t new_capacity = s->capacity ? s->capacity : MCP_SCENE_INITIAL_OPS;
    while (new_capacity < min_capacity)
        new_capacity *= 2;

    mcp_op* ops = (mcp_op*) realloc(s->ops, (size_t) new_capacity * sizeof(mcp_op));
    if (ops == NULL)
        return false;

    s->ops = ops;
    s->capacity = new_capacity;
    return true;
}

//----------------------------------------------------------------------------------------------------------------------------
uint32_t mcp_scene_append(mcp_scene* s, const mcp_op* op)
{
    if (!mcp_scene_grow_ops(s, s->count + 1))
        return UINT32_MAX;

    mcp_op* op2 = &s->ops[s->count];
    *op2 = *op;
    op2->index = s->count;
    op2->alive = true;
    return s->count++;
}

//----------------------------------------------------------------------------------------------------------------------------
bool mcp_scene_update(mcp_scene* s, uint32_t index, const mcp_op* op)
{
    if (index >= s->count || !s->ops[index].alive)
        return false;

    mcp_op* op2 = &s->ops[index];
    *op2 = *op;
    op2->index = index;
    op2->alive = true;
    return true;
}

//----------------------------------------------------------------------------------------------------------------------------
bool mcp_scene_remove(mcp_scene* s, uint32_t index)
{
    if (index >= s->count || !s->ops[index].alive)
        return false;

    s->ops[index].alive = false;
    return true;
}

//----------------------------------------------------------------------------------------------------------------------------
uint32_t mcp_scene_live_count(const mcp_scene* s)
{
    uint32_t count = 0;
    for (uint32_t i = 0; i < s->count; i++)
        if (s->ops[i].alive)
            count++;
    return count;
}

//----------------------------------------------------------------------------------------------------------------------------
const char* mcp_scene_intern(mcp_scene* s, const char* str)
{
    return mcp_scene_intern_len(s, str, strlen(str));
}

//----------------------------------------------------------------------------------------------------------------------------
const char* mcp_scene_intern_len(mcp_scene* s, const char* data, size_t len)
{
    size_t total = len + 1;
    if (s->text_len + total > s->text_capacity)
    {
        size_t new_capacity = s->text_capacity ? s->text_capacity : MCP_SCENE_INITIAL_TEXT;
        while (new_capacity < s->text_len + total)
            new_capacity *= 2;

        char* text = (char*) realloc(s->text, new_capacity);
        if (text == NULL)
            return NULL;

        // the pool layout (offsets) is unchanged, only the base may move : rebase the pointers
        // held by the text ops, or they would dangle after the move
        if (s->text != NULL)
        {
            for (uint32_t i = 0; i < s->count; i++)
            {
                if (s->ops[i].type == MCP_OP_TEXT && s->ops[i].text != NULL)
                    s->ops[i].text = text + ((size_t) (s->ops[i].text - s->text));
            }
        }

        s->text = text;
        s->text_capacity = new_capacity;
    }

    char* at = s->text + s->text_len;
    memcpy(at, data, len);
    at[len] = '\0';
    s->text_len += total;
    return at;
}

//----------------------------------------------------------------------------------------------------------------------------
bool mcp_scene_is_balanced(const mcp_scene* s)
{
    int groups = 0;
    int clips = 0;
    for (uint32_t i = 0; i < s->count; i++)
    {
        const mcp_op* op = &s->ops[i];
        if (!op->alive)
            continue;

        switch(op->type)
        {
        case MCP_OP_BEGIN_GROUP : groups++; break;
        case MCP_OP_END_GROUP :
            if (groups == 0)
                return false;
            groups--;
            break;
        case MCP_OP_PUSH_CLIPRECT :
        case MCP_OP_PUSH_CLIPRECT_ROUNDED :
        case MCP_OP_PUSH_CLIPDISC : clips++; break;
        case MCP_OP_POP_CLIP :
            if (clips == 0)
                return false;
            clips--;
            break;
        default: break;
        }
    }
    return groups == 0 && clips == 0;
}

//----------------------------------------------------------------------------------------------------------------------------
bool mcp_scene_replay(struct onedraw* r, const mcp_scene* s, mcp_transform t)
{
    if (!mcp_scene_is_balanced(s))
        return false;

    for (uint32_t i = 0; i < s->count; i++)
    {
        const mcp_op* op = &s->ops[i];
        if (!op->alive)
            continue;

        const float* f = op->f;
        switch(op->type)
        {
        case MCP_OP_DISC :
            od_draw_disc(r, mcp_tx_x(t, f[0]), mcp_tx_y(t, f[1]), mcp_tx_s(t, f[2]), op->color);
            break;
        case MCP_OP_RING :
            od_draw_ring(r, mcp_tx_x(t, f[0]), mcp_tx_y(t, f[1]), mcp_tx_s(t, f[2]), mcp_tx_s(t, f[3]), op->color);
            break;
        case MCP_OP_DISC_GRADIENT :
            od_draw_disc_gradient(r, mcp_tx_x(t, f[0]), mcp_tx_y(t, f[1]), mcp_tx_s(t, f[2]), op->color, op->color2);
            break;
        case MCP_OP_BOX :
            od_draw_box(r, mcp_tx_x(t, f[0]), mcp_tx_y(t, f[1]), mcp_tx_x(t, f[2]), mcp_tx_y(t, f[3]), mcp_tx_s(t, f[4]), op->color);
            break;
        case MCP_OP_BLURRED_BOX :
            od_draw_blurred_box(r, mcp_tx_x(t, f[0]), mcp_tx_y(t, f[1]), mcp_tx_s(t, f[2]), mcp_tx_s(t, f[3]), mcp_tx_s(t, f[4]), op->color);
            break;
        case MCP_OP_LINE :
            od_draw_line(r, mcp_tx_x(t, f[0]), mcp_tx_y(t, f[1]), mcp_tx_x(t, f[2]), mcp_tx_y(t, f[3]), mcp_tx_s(t, f[4]), op->color);
            break;
        case MCP_OP_CAPSULE :
            od_draw_capsule(r, mcp_tx_x(t, f[0]), mcp_tx_y(t, f[1]), mcp_tx_x(t, f[2]), mcp_tx_y(t, f[3]), mcp_tx_s(t, f[4]), op->color);
            break;
        case MCP_OP_CAPSULE_GRADIENT :
            od_draw_capsule_gradient(r, mcp_tx_x(t, f[0]), mcp_tx_y(t, f[1]), mcp_tx_x(t, f[2]), mcp_tx_y(t, f[3]), mcp_tx_s(t, f[4]), op->color, op->color2);
            break;
        case MCP_OP_ORIENTED_BOX :
            od_draw_oriented_box(r, mcp_tx_x(t, f[0]), mcp_tx_y(t, f[1]), mcp_tx_x(t, f[2]), mcp_tx_y(t, f[3]),
                                 mcp_tx_s(t, f[4]), mcp_tx_s(t, f[5]), op->color);
            break;
        case MCP_OP_ORIENTED_RECT :
            od_draw_oriented_rect(r, mcp_tx_x(t, f[0]), mcp_tx_y(t, f[1]), mcp_tx_x(t, f[2]), mcp_tx_y(t, f[3]),
                                  mcp_tx_s(t, f[4]), mcp_tx_s(t, f[5]), mcp_tx_s(t, f[6]), op->color);
            break;
        case MCP_OP_ELLIPSE :
            od_draw_ellipse(r, mcp_tx_x(t, f[0]), mcp_tx_y(t, f[1]), mcp_tx_x(t, f[2]), mcp_tx_y(t, f[3]), mcp_tx_s(t, f[4]), op->color);
            break;
        case MCP_OP_ELLIPSE_RING :
            od_draw_ellipse_ring(r, mcp_tx_x(t, f[0]), mcp_tx_y(t, f[1]), mcp_tx_x(t, f[2]), mcp_tx_y(t, f[3]),
                                 mcp_tx_s(t, f[4]), mcp_tx_s(t, f[5]), op->color);
            break;
        case MCP_OP_TRIANGLE :
        {
            float v[6] = { mcp_tx_x(t, f[0]), mcp_tx_y(t, f[1]), mcp_tx_x(t, f[2]), mcp_tx_y(t, f[3]), mcp_tx_x(t, f[4]), mcp_tx_y(t, f[5]) };
            od_draw_triangle(r, v, mcp_tx_s(t, f[6]), op->color);
            break;
        }
        case MCP_OP_TRIANGLE_RING :
        {
            float v[6] = { mcp_tx_x(t, f[0]), mcp_tx_y(t, f[1]), mcp_tx_x(t, f[2]), mcp_tx_y(t, f[3]), mcp_tx_x(t, f[4]), mcp_tx_y(t, f[5]) };
            od_draw_triangle_ring(r, v, mcp_tx_s(t, f[6]), mcp_tx_s(t, f[7]), op->color);
            break;
        }
        case MCP_OP_SECTOR :
            od_draw_sector(r, mcp_tx_x(t, f[0]), mcp_tx_y(t, f[1]), mcp_tx_s(t, f[2]), f[3], f[4], op->color);
            break;
        case MCP_OP_SECTOR_RING :
            od_draw_sector_ring(r, mcp_tx_x(t, f[0]), mcp_tx_y(t, f[1]), mcp_tx_s(t, f[2]), f[3], f[4], mcp_tx_s(t, f[5]), op->color);
            break;
        case MCP_OP_ARC :
            od_draw_arc(r, mcp_tx_x(t, f[0]), mcp_tx_y(t, f[1]), f[2], f[3], f[4], mcp_tx_s(t, f[5]), mcp_tx_s(t, f[6]), op->color);
            break;
        case MCP_OP_QUADRATIC_BEZIER :
        {
            float cp[6] = { mcp_tx_x(t, f[0]), mcp_tx_y(t, f[1]), mcp_tx_x(t, f[2]), mcp_tx_y(t, f[3]), mcp_tx_x(t, f[4]), mcp_tx_y(t, f[5]) };
            od_draw_quadratic_bezier(r, cp, mcp_tx_s(t, f[6]), op->color);
            break;
        }
        case MCP_OP_CUBIC_BEZIER :
        {
            float cp[8] = { mcp_tx_x(t, f[0]), mcp_tx_y(t, f[1]), mcp_tx_x(t, f[2]), mcp_tx_y(t, f[3]),
                           mcp_tx_x(t, f[4]), mcp_tx_y(t, f[5]), mcp_tx_x(t, f[6]), mcp_tx_y(t, f[7]) };
            od_draw_cubic_bezier(r, cp, mcp_tx_s(t, f[8]), op->color);
            break;
        }
        case MCP_OP_CHAR :
            od_draw_char(r, mcp_tx_x(t, f[0]), mcp_tx_y(t, f[1]), op->c, op->color);
            break;
        case MCP_OP_TEXT :
            od_draw_text(r, mcp_tx_x(t, f[0]), mcp_tx_y(t, f[1]), op->text, op->color);
            break;
        case MCP_OP_DASHED_LINE :
            od_draw_dashed_line(r, mcp_tx_x(t, f[0]), mcp_tx_y(t, f[1]), mcp_tx_x(t, f[2]), mcp_tx_y(t, f[3]), mcp_tx_s(t, f[4]), op->dash, op->color);
            break;
        case MCP_OP_DASHED_RING :
            od_draw_dashed_ring(r, mcp_tx_x(t, f[0]), mcp_tx_y(t, f[1]), mcp_tx_s(t, f[2]), mcp_tx_s(t, f[3]), op->dash, op->color);
            break;
        case MCP_OP_DASHED_BOX :
            od_draw_dashed_box(r, mcp_tx_x(t, f[0]), mcp_tx_y(t, f[1]), mcp_tx_x(t, f[2]), mcp_tx_y(t, f[3]),
                               mcp_tx_s(t, f[4]), mcp_tx_s(t, f[5]), op->dash, op->color);
            break;
        case MCP_OP_DASHED_ORIENTED_BOX :
            od_draw_dashed_oriented_box(r, mcp_tx_x(t, f[0]), mcp_tx_y(t, f[1]), mcp_tx_x(t, f[2]), mcp_tx_y(t, f[3]),
                                        mcp_tx_s(t, f[4]), mcp_tx_s(t, f[5]), mcp_tx_s(t, f[6]), op->dash, op->color);
            break;
        case MCP_OP_DASHED_ELLIPSE_RING :
            od_draw_dashed_ellipse_ring(r, mcp_tx_x(t, f[0]), mcp_tx_y(t, f[1]), mcp_tx_x(t, f[2]), mcp_tx_y(t, f[3]),
                                        mcp_tx_s(t, f[4]), mcp_tx_s(t, f[5]), op->dash, op->color);
            break;
        case MCP_OP_DASHED_TRIANGLE_RING :
        {
            float v[6] = { mcp_tx_x(t, f[0]), mcp_tx_y(t, f[1]), mcp_tx_x(t, f[2]), mcp_tx_y(t, f[3]), mcp_tx_x(t, f[4]), mcp_tx_y(t, f[5]) };
            od_draw_dashed_triangle_ring(r, v, mcp_tx_s(t, f[6]), op->dash, op->color);
            break;
        }
        case MCP_OP_BEGIN_GROUP :
            od_begin_group(r, op->smoothblend, f[0], mcp_tx_s(t, f[1]));
            break;
        case MCP_OP_END_GROUP :
            od_end_group(r, op->color);
            break;
        case MCP_OP_PUSH_CLIPRECT :
            od_push_cliprect(r, mcp_tx_x(t, f[0]), mcp_tx_y(t, f[1]), mcp_tx_x(t, f[2]), mcp_tx_y(t, f[3]));
            break;
        case MCP_OP_PUSH_CLIPRECT_ROUNDED :
            od_push_cliprect_rounded(r, mcp_tx_x(t, f[0]), mcp_tx_y(t, f[1]), mcp_tx_x(t, f[2]), mcp_tx_y(t, f[3]), mcp_tx_s(t, f[4]));
            break;
        case MCP_OP_PUSH_CLIPDISC :
            od_push_clipdisc(r, mcp_tx_x(t, f[0]), mcp_tx_y(t, f[1]), mcp_tx_s(t, f[2]));
            break;
        case MCP_OP_POP_CLIP :
            od_pop_clip(r);
            break;
        default:
            break;
        }
    }
    return true;
}
