#include "../tool/mcp/mcp.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); fails++; } } while (0)

static mcp_scene s;
static mcp_parse_result r;
static char err[256];

static bool call(const char* json)
{
    size_t len = strlen(json);
    char* buf = (char*) malloc(len + 1);
    memcpy(buf, json, len + 1);
    bool ok = mcp_parse_tool_call(&s, buf, len, &r, err, sizeof(err));
    free(buf);
    if (!ok)
        printf("  err: %s\n", err);
    return ok;
}

int main(void)
{
    mcp_scene_init(&s);

    // color parser
    {
        draw_color c = 0;
        CHECK(mcp_parse_color("#ff8800", &c) && c == 0xffff8800);
        CHECK(mcp_parse_color("#ff880012", &c) && c == 0x12ff8800);
        CHECK(!mcp_parse_color("#ff800", &c));
        CHECK(!mcp_parse_color("ff8800", &c));
        CHECK(!mcp_parse_color("#zz8800", &c));
        CHECK(!mcp_parse_color(NULL, &c));
    }

    // clear
    CHECK(call("{\"cmd\":\"clear\",\"width\":960,\"height\":640,\"background\":\"#203040\"}"));
    CHECK(s.canvas_w == 960.f && s.canvas_h == 640.f && s.background == 0xff203040);
    CHECK(r.cmd == MCP_CMD_CLEAR && r.live == 0);
    CHECK(call("{\"cmd\":\"clear\"}"));
    CHECK(s.canvas_w == 1024.f && s.canvas_h == 768.f && s.background == 0xffebecdc);
    CHECK(!call("{\"cmd\":\"clear\",\"width\":0}"));
    CHECK(!call("{\"cmd\":\"clear\",\"background\":\"nope\"}"));

    // draw (example from the plan)
    CHECK(call("{\"cmd\":\"draw\",\"primitives\":["
        "{\"type\":\"box\",\"x0\":40,\"y0\":40,\"x1\":400,\"y1\":240,\"radius\":12,\"color\":\"#2485a6\"},"
        "{\"type\":\"disc\",\"cx\":500,\"cy\":140,\"radius\":80,\"color\":\"#c65046\"},"
        "{\"type\":\"text\",\"x\":40,\"y\":280,\"text\":\"Hello onedraw\",\"color\":\"#232228\"}"
        "]}"));
    CHECK(r.index == 0 && r.count == 3 && r.live == 3);
    CHECK(s.ops[0].type == MCP_OP_BOX && s.ops[0].f[0] == 40.f && s.ops[0].f[4] == 12.f && s.ops[0].color == 0xff2485a6);
    CHECK(s.ops[1].type == MCP_OP_DISC && s.ops[1].f[0] == 500.f && s.ops[1].f[1] == 140.f && s.ops[1].f[2] == 80.f && s.ops[1].color == 0xffc65046);
    CHECK(s.ops[2].type == MCP_OP_TEXT && s.ops[2].f[0] == 40.f && s.ops[2].f[1] == 280.f && s.ops[2].text != NULL && strcmp(s.ops[2].text, "Hello onedraw") == 0);

    // remove leaves a hole; draw after it keeps stable indices
    CHECK(call("{\"cmd\":\"remove\",\"index\":1}"));
    CHECK(!s.ops[1].alive && r.live == 2);
    CHECK(call("{\"cmd\":\"draw\",\"primitives\":[{\"type\":\"ring\",\"cx\":1,\"cy\":2,\"radius\":3,\"thickness\":4}]}"));
    CHECK(r.index == 3 && r.count == 1 && r.live == 3);

    // update by index; type may change; hole / out-of-range / missing keys fail
    CHECK(call("{\"cmd\":\"update\",\"index\":2,\"type\":\"disc\",\"cx\":560,\"cy\":140}"));
    CHECK(r.index == 2 && s.ops[2].type == MCP_OP_DISC && s.ops[2].f[0] == 560.f && s.ops[2].f[1] == 140.f);
    CHECK(!call("{\"cmd\":\"update\",\"index\":1,\"type\":\"disc\"}"));
    CHECK(!call("{\"cmd\":\"update\",\"index\":99,\"type\":\"disc\"}"));
    CHECK(!call("{\"cmd\":\"update\",\"index\":0}"));

    // screenshot sizes
    CHECK(call("{\"cmd\":\"screenshot\",\"width\":1280,\"height\":720}"));
    CHECK(r.shot_w == 1280 && r.shot_h == 720);
    CHECK(call("{\"cmd\":\"screenshot\"}"));
    CHECK(r.shot_w == 0 && r.shot_h == 0);
    CHECK(!call("{\"cmd\":\"screenshot\",\"width\":4097,\"height\":10}"));
    CHECK(!call("{\"cmd\":\"screenshot\",\"width\":100}"));
    CHECK(!call("{\"cmd\":\"screenshot\",\"width\":0,\"height\":0}"));

    // every primitive type of plan §6 parses and lands in the right op
    CHECK(call("{\"cmd\":\"clear\"}"));
    const char* all =
        "{\"type\":\"disc\",\"cx\":1,\"cy\":2,\"radius\":3},"
        "{\"type\":\"ring\",\"cx\":1,\"cy\":2,\"radius\":3,\"thickness\":4},"
        "{\"type\":\"disc_gradient\",\"cx\":1,\"cy\":2,\"radius\":3,\"color\":\"#ff0000\",\"color2\":\"#00ff00\"},"
        "{\"type\":\"box\",\"x0\":1,\"y0\":2,\"x1\":3,\"y1\":4,\"radius\":5},"
        "{\"type\":\"blurred_box\",\"cx\":1,\"cy\":2,\"half_width\":3,\"half_height\":4,\"roundness\":5},"
        "{\"type\":\"line\",\"ax\":1,\"ay\":2,\"bx\":3,\"by\":4,\"width\":5},"
        "{\"type\":\"capsule\",\"ax\":1,\"ay\":2,\"bx\":3,\"by\":4,\"radius\":5},"
        "{\"type\":\"capsule_gradient\",\"ax\":1,\"ay\":2,\"bx\":3,\"by\":4,\"radius\":5,\"color\":\"#0000ff\",\"color2\":\"#ffff00\"},"
        "{\"type\":\"oriented_box\",\"ax\":1,\"ay\":2,\"bx\":3,\"by\":4,\"width\":5,\"roundness\":6},"
        "{\"type\":\"oriented_rect\",\"ax\":1,\"ay\":2,\"bx\":3,\"by\":4,\"width\":5,\"roundness\":6,\"thickness\":7},"
        "{\"type\":\"ellipse\",\"ax\":1,\"ay\":2,\"bx\":3,\"by\":4,\"width\":5},"
        "{\"type\":\"ellipse_ring\",\"ax\":1,\"ay\":2,\"bx\":3,\"by\":4,\"width\":5,\"thickness\":6},"
        "{\"type\":\"triangle\",\"ax\":1,\"ay\":2,\"bx\":3,\"by\":4,\"cx\":5,\"cy\":6,\"roundness\":7},"
        "{\"type\":\"triangle_ring\",\"ax\":1,\"ay\":2,\"bx\":3,\"by\":4,\"cx\":5,\"cy\":6,\"roundness\":7,\"thickness\":8},"
        "{\"type\":\"sector\",\"cx\":1,\"cy\":2,\"radius\":3,\"start_angle\":0.5,\"sweep_angle\":1.2},"
        "{\"type\":\"sector_ring\",\"cx\":1,\"cy\":2,\"radius\":3,\"start_angle\":0.5,\"sweep_angle\":1.2,\"thickness\":6},"
        "{\"type\":\"arc\",\"cx\":1,\"cy\":2,\"dx\":0.3,\"dy\":0.9,\"aperture\":2,\"radius\":4,\"thickness\":5},"
        "{\"type\":\"quadratic_bezier\",\"ax\":1,\"ay\":2,\"cx\":3,\"cy\":4,\"bx\":5,\"by\":6,\"width\":7},"
        "{\"type\":\"cubic_bezier\",\"ax\":1,\"ay\":2,\"c1x\":3,\"c1y\":4,\"c2x\":5,\"c2y\":6,\"bx\":7,\"by\":8,\"width\":9},"
        "{\"type\":\"char\",\"x\":1,\"y\":2,\"c\":\"A\"},"
        "{\"type\":\"text\",\"x\":1,\"y\":2,\"text\":\"hi\"},"
        "{\"type\":\"dashed_line\",\"ax\":1,\"ay\":2,\"bx\":3,\"by\":4,\"width\":5,\"dash\":{\"length\":6,\"gap\":7,\"offset\":8,\"round_caps\":true}},"
        "{\"type\":\"dashed_ring\",\"cx\":1,\"cy\":2,\"radius\":3,\"thickness\":4},"
        "{\"type\":\"dashed_box\",\"x0\":1,\"y0\":2,\"x1\":3,\"y1\":4,\"radius\":5,\"thickness\":6,\"dash\":{\"length\":9}},"
        "{\"type\":\"dashed_oriented_box\",\"ax\":1,\"ay\":2,\"bx\":3,\"by\":4,\"width\":5,\"roundness\":6,\"thickness\":7},"
        "{\"type\":\"dashed_ellipse_ring\",\"ax\":1,\"ay\":2,\"bx\":3,\"by\":4,\"width\":5,\"thickness\":6},"
        "{\"type\":\"dashed_triangle_ring\",\"ax\":1,\"ay\":2,\"bx\":3,\"by\":4,\"cx\":5,\"cy\":6,\"thickness\":7},"
        "{\"type\":\"begin_group\",\"smooth_value\":0.5,\"outline_width\":2,\"smooth\":true},"
        "{\"type\":\"end_group\",\"color\":\"#123456\"},"
        "{\"type\":\"push_cliprect\",\"min_x\":1,\"min_y\":2,\"max_x\":3,\"max_y\":4},"
        "{\"type\":\"push_cliprect_rounded\",\"min_x\":1,\"min_y\":2,\"max_x\":3,\"max_y\":4,\"radius\":5},"
        "{\"type\":\"push_clipdisc\",\"cx\":1,\"cy\":2,\"radius\":3},"
        "{\"type\":\"pop_clip\"},"
        "{\"type\":\"pop_clip\"},"
        "{\"type\":\"pop_clip\"}";
    char json[8192];
    snprintf(json, sizeof(json), "{\"cmd\":\"draw\",\"primitives\":[%s]}", all);
    CHECK(call(json));
    CHECK(r.index == 0 && r.count == 35 && r.live == 35);

    // spot-check the op mappings (op index == position in the array above)
    CHECK(s.ops[2].type == MCP_OP_DISC_GRADIENT && s.ops[2].color == 0xffff0000 && s.ops[2].color2 == 0xff00ff00);
    CHECK(s.ops[7].type == MCP_OP_CAPSULE_GRADIENT && s.ops[7].color == 0xff0000ff && s.ops[7].color2 == 0xffffff00);
    CHECK(s.ops[9].type == MCP_OP_ORIENTED_RECT && s.ops[9].f[4] == 5.f && s.ops[9].f[5] == 6.f && s.ops[9].f[6] == 7.f);
    CHECK(s.ops[14].type == MCP_OP_SECTOR && s.ops[14].f[3] == 0.5f && s.ops[14].f[4] == 1.2f);
    CHECK(s.ops[16].type == MCP_OP_ARC && s.ops[16].f[2] == 0.3f && s.ops[16].f[6] == 5.f);
    CHECK(s.ops[18].type == MCP_OP_CUBIC_BEZIER && s.ops[18].f[8] == 9.f);
    CHECK(s.ops[19].type == MCP_OP_CHAR && s.ops[19].c == 'A');
    CHECK(s.ops[20].type == MCP_OP_TEXT && strcmp(s.ops[20].text, "hi") == 0);
    CHECK(s.ops[21].type == MCP_OP_DASHED_LINE && s.ops[21].dash.dash_length == 6.f && s.ops[21].dash.gap_length == 7.f
          && s.ops[21].dash.offset == 8.f && s.ops[21].dash.round_caps);
    CHECK(s.ops[23].type == MCP_OP_DASHED_BOX && s.ops[23].dash.dash_length == 9.f && s.ops[23].dash.gap_length == 0.f && !s.ops[23].dash.round_caps);
    CHECK(s.ops[27].type == MCP_OP_BEGIN_GROUP && s.ops[27].smoothblend && s.ops[27].f[0] == 0.5f && s.ops[27].f[1] == 2.f);
    CHECK(s.ops[28].type == MCP_OP_END_GROUP && s.ops[28].color == 0xff123456);
    CHECK(s.ops[30].type == MCP_OP_PUSH_CLIPRECT_ROUNDED && s.ops[30].f[4] == 5.f);
    CHECK(s.ops[32].type == MCP_OP_POP_CLIP);
    CHECK(mcp_scene_is_balanced(&s));

    // malformed primitives
    CHECK(!call("{\"cmd\":\"draw\",\"primitives\":[{\"type\":\"nope\"}]}"));
    CHECK(!call("{\"cmd\":\"draw\",\"primitives\":[{\"type\":\"char\",\"x\":1,\"y\":2,\"c\":\"ab\"}]}"));
    CHECK(!call("{\"cmd\":\"draw\",\"primitives\":[{\"type\":\"text\",\"x\":1,\"y\":2}]}"));
    CHECK(!call("{\"cmd\":\"draw\",\"primitives\":[{\"type\":\"disc\",\"cx\":\"five\"}]}"));
    CHECK(!call("{\"cmd\":\"draw\",\"primitives\":[{\"type\":\"disc\",\"color\":\"#123\"}]}"));
    CHECK(!call("{\"cmd\":\"draw\",\"primitives\":[{\"type\":\"dashed_line\",\"ax\":1,\"ay\":2,\"bx\":3,\"by\":4,\"width\":5,\"dash\":{\"length\":\"x\"}}]}"));
    CHECK(!call("{\"cmd\":\"draw\",\"primitives\":[1]}"));
    CHECK(!call("{\"cmd\":\"draw\"}"));
    CHECK(!call("{\"cmd\":\"nope\"}"));
    CHECK(!call("[1,2]"));
    CHECK(!call("{\"cmd\":\"clear\""));

    // the text pool survives interning across a realloc (the pointers must be rebased)
    CHECK(call("{\"cmd\":\"clear\"}"));
    for (int i = 0; i < 40; i++)
    {
        snprintf(json, sizeof(json), "{\"cmd\":\"draw\",\"primitives\":[{\"type\":\"text\",\"x\":0,\"y\":0,\"text\":\"%s\"}]}",
                 "0123456789012345678901234567890123456789");
        CHECK(call(json));
    }
    CHECK(s.ops[0].text != NULL && strcmp(s.ops[0].text, "0123456789012345678901234567890123456789") == 0);
    CHECK(s.ops[39].text != NULL && strcmp(s.ops[39].text, "0123456789012345678901234567890123456789") == 0);

    printf(fails ? "parse checks: %d FAILURES\n" : "parse checks: all ok\n", fails);
    return fails ? 1 : 0;
}
