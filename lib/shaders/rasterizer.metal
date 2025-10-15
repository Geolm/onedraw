#include <metal_stdlib>
#include "common.h"

// ---------------------------------------------------------------------------------------------------------------------------
// SDF functions
// ---------------------------------------------------------------------------------------------------------------------------

inline float erf(float x) {return sign(x) * sqrt(1.0 - exp2(-1.787776 * x * x));}
inline float sd_disc(float2 position, float2 center, float radius) {return length(center-position) - radius;}

//-----------------------------------------------------------------------------
inline float sd_segment(float2 position, float2 a, float2 b)
{
    float2 pa = position-a, ba = b-a;
    float h = clamp( dot(pa,ba)/dot(ba,ba), 0.f, 1.f);
    return length(pa - ba*h);
}

//-----------------------------------------------------------------------------
// based on https://www.shadertoy.com/view/NsVSWy
//   [blur_radius] is half of the roundness
// returns a float2
//      .x = distance to box
//      .y = gaussian blur value (alpha)
inline float2 sd_gaussian_box(float2 position, float2 box_center, float2 box_size, float radius)
{
    position -= box_center;
    float2 d = abs(position) - box_size;
    float sd = length(max(d,0.0)) + min(max(d.x,d.y),0.0) - radius;
    float blur_radius = radius * 0.5f;

    float u = erf((position.x + box_size.x) / blur_radius) - erf((position.x - box_size.x) / blur_radius);
    float v = erf((position.y + box_size.y) / blur_radius) - erf((position.y - box_size.y) / blur_radius);
    return float2(sd, u * v / 4.0);
}

//-----------------------------------------------------------------------------
inline float sd_aabox(float2 position, float2 box_center, float2 half_extents, float radius)
{
    position -= box_center;
    position = abs(position) - half_extents + radius;
    return length(max(position, 0.0)) + min(max(position.x, position.y), 0.0) - radius;
}

//-----------------------------------------------------------------------------
inline float sd_oriented_box(float2 position, float2 a, float2 b, float width)
{
    float l = length(b-a);
    float2  d = (b-a)/l;
    float2  q = (position-(a+b)*0.5);
    q = float2x2(d.x,-d.y,d.y,d.x)*q;
    q = abs(q)-float2(l,width)*0.5;
    return length(max(q,0.0)) + min(max(q.x,q.y),0.0);
}

//-----------------------------------------------------------------------------
inline float sd_triangle(float2 p, float2 p0, float2 p1, float2 p2 )
{
    float2 e0 = p1 - p0;
    float2 e1 = p2 - p1;
    float2 e2 = p0 - p2;

    float2 v0 = p - p0;
    float2 v1 = p - p1;
    float2 v2 = p - p2;

    float2 pq0 = v0 - e0*saturate(dot(v0,e0)/dot(e0,e0));
    float2 pq1 = v1 - e1*saturate(dot(v1,e1)/dot(e1,e1));
    float2 pq2 = v2 - e2*saturate(dot(v2,e2)/dot(e2,e2));
    
    float s = e0.x*e2.y - e0.y*e2.x;
    float2 d = min(min(float2(dot(pq0, pq0 ), s*(v0.x*e0.y-v0.y*e0.x)),
                       float2(dot(pq1, pq1 ), s*(v1.x*e1.y-v1.y*e1.x))),
                       float2(dot(pq2, pq2 ), s*(v2.x*e2.y-v2.y*e2.x)));

    return -sqrt(d.x)*sign(d.y);
}

//-----------------------------------------------------------------------------
// based on https://www.shadertoy.com/view/tt3yz7
inline float sd_ellipse(float2 p, float2 e)
{
    float2 pAbs = abs(p);
    float2 ei = 1.f / e;
    float2 e2 = e*e;
    float2 ve = ei * float2(e2.x - e2.y, e2.y - e2.x);
    
    float2 t = float2(0.70710678118654752f, 0.70710678118654752f);

    // hopefully unroll by the compiler
    for (int i = 0; i < 3; i++) 
    {
        float2 v = ve*t*t*t;
        float2 u = normalize(pAbs - v) * length(t * e - v);
        float2 w = ei * (v + u);
        t = normalize(saturate(w));
    }
    
    float2 nearestAbs = t * e;
    float dist = length(pAbs - nearestAbs);
    return dot(pAbs, pAbs) < dot(nearestAbs, nearestAbs) ? -dist : dist;
}

//-----------------------------------------------------------------------------
inline float sd_oriented_ellipse(float2 position, float2 a, float2 b, float width)
{
    float height = length(b-a);
    float2  axis = (b-a)/height;
    float2  position_translated = (position-(a+b)*.5f);
    float2 position_boxspace = float2x2(axis.x,-axis.y, axis.y, axis.x)*position_translated;
    return sd_ellipse(position_boxspace, float2(height * .5f, width * .5f));
}

//-----------------------------------------------------------------------------
inline float sd_oriented_pie(float2 position, float2 center, float2 direction, float2 aperture, float radius)
{
    direction = -skew(direction);
    position -= center;
    position = float2x2(direction.x,-direction.y, direction.y, direction.x) * position;
    position.x = abs(position.x);
    float l = length(position) - radius;
	float m = length(position - aperture*clamp(dot(position,aperture),0.f,radius));
    return max(l,m*sign(aperture.y*position.x - aperture.x*position.y));
}

//-----------------------------------------------------------------------------
inline float sd_oriented_ring(float2 position, float2 center, float2 direction, float2 aperture, float radius, float thickness)
{
    direction = -skew(direction);
    position -= center;
    position = float2x2(direction.x,-direction.y, direction.y, direction.x) * position;
    position.x = abs(position.x);
    position = float2x2(aperture.y,aperture.x,-aperture.x,aperture.y)*position;
    return max(abs(length(position)-radius)-thickness*0.5,length(float2(position.x,max(0.0,abs(radius-position.y)-thickness*0.5)))*sign(position.x) );
}

//-----------------------------------------------------------------------------
inline float sd_uneven_capsule(float2 p, float2 pa, float2 pb, float ra, float rb)
{
    p  -= pa;
    pb -= pa;
    float h = dot(pb,pb);
    float2  q = float2( dot(p,float2(pb.y,-pb.x)), dot(p,pb) )/h;
    
    q.x = abs(q.x);
    float b = ra-rb;
    float2  c = float2(sqrt(h-b*b),b);
    
    float k = cross2(c,q);
    float m = dot(c,q);
    float n = dot(q,q);

         if( k < 0.0 ) return sqrt(h*(n            )) - ra;
    else if( k > c.x ) return sqrt(h*(n+1.f-2.f*q.y)) - rb;
                       return m                       - ra;
}

//-----------------------------------------------------------------------------
// based on https://www.shadertoy.com/view/wtSyWc to avoid middle line from the iq sdf
inline float sd_trapezoid(float2 p, float2 a, float2 b, float ra, float rb)
{
    float2 pa = p - a;
    float2 ba = b - a;
    float baba = dot(ba, ba);
    float x = abs(dot(float2(-pa.y, pa.x), ba)) / sqrt(baba);
    float paba = dot(pa, ba) / baba;
    float rba = rb - ra;
    float cax = max(0., x - ((paba < 0.5) ? ra : rb));
    float cay = abs(paba - 0.5) - 0.5;
    float f = saturate((rba*(x - ra) + paba * baba) / (rba * rba + baba));
    float cbx = x - ra - f * rba;
    float cby = paba - f;
    return sign(max(cbx,cay)) * sqrt(min(cax*cax + cay * cay*baba, cbx*cbx + cby * cby*baba));
}

// ---------------------------------------------------------------------------------------------------------------------------
// smooth minimum : quadratic polynomial
//
// note : a primitive is on top of b primitive. Also it handle a anti-aliased primitive going over another primitive and keeping 
//        the right width of anti-aliasing 
//
// returns
//  .x = smallest distance
//  .y = blend factor between [0; 1]
float2 smooth_minimum(float a, float b, float k)
{
    b = max(b, 0.f);    // a is always on top
    if (k>0.f)
    {
        float h = max( k-abs(a-b), 0.0f )/k;
        float m = h*h*h*0.5;
        float s = m*k*(1.0f/3.0f); 
        return (a<b) ? float2(a-s, 0.f) : float2(b-s, 1.f - smoothstep(-k, 0.f, b-a));
    }
    else
    {
        // "hard" min
        return float2(min(a, b),  (a<b) ? 0.f : 1.f);
    }
}

// ---------------------------------------------------------------------------------------------------------------------------
float smooth_substraction(float d1, float d2, float k)
{
    if (k>0.f)
    {
        float h = saturate(0.5f - 0.5f*(d2+d1)/k);
        return mix(d1, -d2, h) + k*h*(1.0f-h);
    }
    else
    {
        return max(-d2, d1);
    }
}


struct vs_out
{
    float4 pos [[position]];
    uint16_t tile_index [[flat]];
};

// ---------------------------------------------------------------------------------------------------------------------------
// vertex shader
// ---------------------------------------------------------------------------------------------------------------------------
vertex vs_out tile_vs(uint instance_id [[instance_id]],
                      uint vertex_id [[vertex_id]],
                      constant draw_cmd_arguments& input [[buffer(0)]],
                      constant uint16_t* tile_indices [[buffer(1)]])
{
    vs_out out;

    uint16_t tile_index = tile_indices[instance_id];
    uint16_t tile_x = tile_index % input.num_tile_width;
    uint16_t tile_y = tile_index / input.num_tile_width;
    
    float2 screen_pos = float2(vertex_id&1, vertex_id>>1);
    screen_pos += float2(tile_x, tile_y);
    screen_pos *= TILE_SIZE;

    float2 clipspace_pos = screen_pos * input.screen_div;
    clipspace_pos = (clipspace_pos * 2.f) - 1.f;
    clipspace_pos.y = -clipspace_pos.y;

    out.pos = float4(clipspace_pos.xy, 0.f, 1.0f);
    out.tile_index = tile_index;

    return out;
}

half4 accumulate_color(half4 color, half4 backbuffer)
{
    half3 rgb = mix(backbuffer.rgb, color.rgb, color.a);
    return half4(rgb, 1.h);
}

// ---------------------------------------------------------------------------------------------------------------------------
// fragment shader
// ---------------------------------------------------------------------------------------------------------------------------
fragment half4 tile_fs(vs_out in [[stage_in]],
                       constant draw_cmd_arguments& input [[buffer(0)]],
                       device tiles_data& tiles [[buffer(1)]])
{
    half4 output = input.culling_debug ? half4(0.f, 0.f, 1.0f, 1.0f) : half4(input.clear_color);
    uint32_t node_index = tiles.head[in.tile_index];
    if (node_index == INVALID_INDEX)
        return output;

    float previous_distance;
    half4 previous_color;
    float combination_smoothness;
    bool combining = false;

    half4 outline_color = unpack_unorm4x8_to_half(input.outline_color.packed_data);
    float outline_full = -input.outline_width;
    float outline_start = outline_full - input.aa_width;

    while (node_index != INVALID_INDEX)
    {
        const tile_node node = tiles.nodes[node_index];
        constant draw_command& cmd = input.commands[node.command_index];
        constant clip_rect& clip = input.clips[cmd.clip_index];
        const uint32_t data_index = quad_broadcast(cmd.data_index, 0);
        const uint8_t quad_type = quad_broadcast(cmd.type, 0);
        half4 cmd_color = unpack_unorm4x8_srgb_to_half(cmd.color.packed_data);

        // check if the pixel is in the clip rect
        if (all(float4(in.pos.xy, clip.max_x, clip.max_y) >= float4(clip.min_x, clip.min_y, in.pos.xy)))
        {
            float distance = 10.f;
            constant float* data = &input.draw_data[data_index];
            command_type type = primitive_get_type(quad_type);

            if (type == combination_begin)
            {
                previous_color = 0.h;
                previous_distance = 100000000.f;
                combination_smoothness = data[0];
                combining = true;
            }
            else
            {
                const primitive_fillmode fillmode =  primitive_get_fillmode(quad_type);

                switch(type)
                {
                case primitive_disc :
                {
                    float2 center = float2(data[0], data[1]);
                    float radius = data[2];
                    distance = sd_disc(in.pos.xy, center, radius);
                    if (fillmode == fill_hollow)
                        distance = abs(distance) - data[3];
                    break;
                }
                case primitive_oriented_box :
                {
                    float2 p0 = float2(data[0], data[1]);
                    float2 p1 = float2(data[2], data[3]);
                    distance = sd_oriented_box(in.pos.xy, p0, p1, data[4]);
                    if (fillmode == fill_hollow)
                        distance = abs(distance);
                    distance -= data[5];
                    break;
                }
                case primitive_ellipse :
                {
                    float2 p0 = float2(data[0], data[1]);
                    float2 p1 = float2(data[2], data[3]);
                    distance = sd_oriented_ellipse(in.pos.xy, p0, p1, data[4]);
                    if (fillmode == fill_hollow)
                        distance = abs(distance) - data[5];
                    break;
                }
                case primitive_aabox:
                {
                    float2 center = float2(data[0], data[1]);
                    float2 half_extents = float2(data[2], data[3]);
                    float radius = data[4];
                    distance = sd_aabox(in.pos.xy, center, half_extents, radius);
                    break;
                }
                case primitive_char:
                {
                    uint glyph_index = cmd.custom_data;
                    if (glyph_index<MAX_GLYPHS)
                    {
                        float2 top_left = float2(data[0], data[1]);
                        constant font_char& g = input.glyphs[glyph_index];
                        float2 uv_topleft = g.uv_topleft;
                        float2 uv_bottomright = g.uv_bottomright;
                        float2 char_size = float2(g.width, g.height);
                        float2 t = (in.pos.xy - top_left) / char_size.xy;

                        if (all(t >= 0.f && t <= 1.f))
                        {
                            float2 uv = mix(uv_topleft, uv_bottomright, t);
                            constexpr sampler s(address::clamp_to_zero, filter::linear );
                            half texel = 1.h - input.font.sample(s, uv).r;
                            distance = texel * input.aa_width;
                        }
                    }

                    break;
                }
                case primitive_triangle:
                {
                    float2 p0 = float2(data[0], data[1]);
                    float2 p1 = float2(data[2], data[3]);
                    float2 p2 = float2(data[4], data[5]);
                    distance = sd_triangle(in.pos.xy, p0, p1, p2);
                    
                    if (fillmode == fill_hollow)
                        distance = abs(distance);
                        
                    distance -= data[6];
                    break;
                }
                case primitive_pie:
                {
                    float2 center = float2(data[0], data[1]);
                    float radius = data[2];
                    float2 direction = float2(data[3], data[4]);
                    float2 aperture = float2(data[5], data[6]);

                    distance = sd_oriented_pie(in.pos.xy, center, direction, aperture, radius);
                    if (fillmode == fill_hollow)
                        distance = abs(distance) - data[7];
                    break;
                }
                case primitive_arc:
                {
                    float2 center = float2(data[0], data[1]);
                    float radius = data[2];
                    float2 direction = float2(data[3], data[4]);
                    float2 aperture = float2(data[5], data[6]);
                    float thickness = data[7];

                    distance = sd_oriented_ring(in.pos.xy, center, direction, aperture, radius, thickness);
                    if (fillmode == fill_hollow)
                        distance = abs(distance) - data[7];
                    break;
                }
                case primitive_uneven_capsule:
                {
                    float2 p0 = float2(data[0], data[1]);
                    float2 p1 = float2(data[2], data[3]);
                    float radius0 = data[4];
                    float radius1 = data[5];

                    distance = sd_uneven_capsule(in.pos.xy, p0, p1, radius0, radius1);
                    if (fillmode == fill_hollow)
                        distance = abs(distance) - data[6];
                    break;
                }
                case primitive_trapezoid:
                {
                    float2 p0 = float2(data[0], data[1]);
                    float2 p1 = float2(data[2], data[3]);
                    float radius0 = data[4];
                    float radius1 = data[5];

                    distance = sd_trapezoid(in.pos.xy, p0, p1, radius0, radius1);
                    if (fillmode == fill_hollow)
                        distance = abs(distance) - data[6];
                    else
                        distance -= data[6];

                    break;
                }
                case primitive_blurred_box:
                {
                    float2 center = float2(data[0], data[1]);
                    float2 size = float2(data[2], data[3]);
                    float roundness = data[4];

                    float2 dist_alpha = sd_gaussian_box(in.pos.xy, center, size, roundness);

                    distance = dist_alpha.x;
                    cmd_color.a *= dist_alpha.y;
                    break;
                }
                default: break;
                }

                half4 color;
                if (type == combination_end)
                {
                    combining = false;
                    color = previous_color;
                    distance = previous_distance;
                }
                else
                {
                    color = cmd_color;

                    if (fillmode == fill_outline && distance >= outline_start)
                    {
                        if (distance >= outline_full && distance <= input.aa_width)
                            color.rgb = outline_color.rgb;
                        else if (distance < outline_full)
                            color.rgb = mix(outline_color.rgb, color.rgb, linearstep(half(outline_full), half(outline_start), half(distance)));
                    }
                }

                // blend distance / color and skip writing output
                if (combining)
                {
                    float smooth_factor = (cmd.op == op_blend) ? combination_smoothness : input.aa_width;
                    switch(cmd.op)
                    {
                    case op_add :
                    case op_blend :
                        {
                            float2 smooth = smooth_minimum(distance, previous_distance, smooth_factor);
                            previous_distance = smooth.x;
                            previous_color = mix(color, previous_color, smooth.y);
                            break;
                        }
                    case op_subtraction :
                        {
                            previous_distance = smooth_substraction(previous_distance, distance, smooth_factor);
                            break;
                        }
                    }
                }
                else
                {
                    half alpha_factor;
                    if (fillmode == fill_outline && type == combination_end)
                    {
                        if (distance > input.aa_width)
                            color.rgb = outline_color.rgb;
                        else
                            color.rgb = mix(outline_color.rgb, color.rgb, linearstep(input.aa_width, 0.f, distance));
                        alpha_factor = linearstep(half(input.aa_width*2+input.outline_width), half(input.aa_width+input.outline_width), half(distance));    // anti-aliasing
                    }
                    else
                        alpha_factor = linearstep(half(input.aa_width), 0.h, half(distance));    // anti-aliasing

                    color.a *= alpha_factor;
                    output = accumulate_color(color, output);
                }
            }
        }
        node_index = node.next;
    }

    return output;
}
