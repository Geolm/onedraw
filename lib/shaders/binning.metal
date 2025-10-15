#include <metal_stdlib>
#include "common.h"

// ---------------------------------------------------------------------------------------------------------------------------
// Collisions functions
// ---------------------------------------------------------------------------------------------------------------------------

struct aabb
{
    float2 min;
    float2 max;
};

aabb aabb_grow(aabb box, float2 amount)
{
    return (aabb) {.min = box.min - amount, .max = box.max + amount};
}

float2 aabb_get_extents(aabb box) {return box.max - box.min;}
float2 aabb_get_center(aabb box) {return (box.min + box.max) * .5f;}
aabb aabb_scale(aabb box, float scale)
{
    float2 half_extents = aabb_get_extents(box) * .5f;
    float2 center = aabb_get_center(box);

    aabb output;
    output.min = center - half_extents * scale;
    output.max = center + half_extents * scale;
    return output;
}

template <class T> T square(T value) {return value*value;}

// ---------------------------------------------------------------------------------------------------------------------------
float3 edge_init(float2 a, float2 b)
{
    float3 edge;
    edge.x = a.y - b.y;
    edge.y = b.x - a.x;
    edge.z = a.x * b.y - a.y * b.x;
    return edge;
}

// ---------------------------------------------------------------------------------------------------------------------------
float edge_distance(float3 e, float2 p)
{
    return e.x * p.x + e.y * p.y + e.z;
}

// ---------------------------------------------------------------------------------------------------------------------------
float edge_sign(float2 p, float2 e0, float2 e1)
{
    return (p.x - e1.x) * (e0.y - e1.y) - (e0.x - e1.x) * (p.y - e1.y);
}

struct obb
{
    float2 axis_i;
    float2 axis_j;
    float2 center;
    float2 extents;
};

// ---------------------------------------------------------------------------------------------------------------------------
obb compute_obb(float2 p0, float2 p1, float width)
{
    obb result;
    result.center = (p0 + p1) * .5f;
    result.axis_j = (p1 - result.center);
    result.extents.y = length(result.axis_j);
    result.axis_j /= result.extents.y;
    result.axis_i = skew(result.axis_j);
    result.extents.x = width * .5f;
    return result;
}

// ---------------------------------------------------------------------------------------------------------------------------
float2 obb_transform(obb obox, float2 point)
{
    point = point - obox.center;
    return float2(abs(dot(obox.axis_i, point)), abs(dot(obox.axis_j, point)));
}

// ---------------------------------------------------------------------------------------------------------------------------
// slab test
bool intersection_aabb_ray(aabb box, float2 origin, float2 direction)
{
    float tmin = 0.f;
    float tmax = 1e10f;
    for (int i = 0; i < 2; i++)
    {
        float inv_dir = 1.f / direction[i];
        float t1 = (box.min[i] - origin[i]) * inv_dir;
        float t2 = (box.max[i] - origin[i]) * inv_dir;

        if (t1 > t2)
        {
            float temp = t1;
            t1 = t2;
            t2 = temp;
        }

        tmin = max(tmin, t1);
        tmax = min(tmax, t2);

        if (tmin > tmax)
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------------
bool intersection_aabb_disc(aabb box, float2 center, float radius)
{
    float2 nearest_point = clamp(center.xy, box.min, box.max);
    return distance_squared(nearest_point, center) < square(radius);
}

// ---------------------------------------------------------------------------------------------------------------------------
bool intersection_aabb_circle(aabb box, float2 center, float radius, float half_width)
{
    if (!intersection_aabb_disc(box, center, radius + half_width))
        return false;

    float2 candidate0 = abs(center.xy - box.min);
    float2 candidate1 = abs(center.xy - box.max);
    float2 furthest_point = max(candidate0, candidate1);

    return length_squared(furthest_point) > square(radius - half_width);
}

// ---------------------------------------------------------------------------------------------------------------------------
bool intersection_aabb_aabb(aabb box0, aabb box1)
{
    return !(box1.max.x < box0.min.x || box0.max.x < box1.min.x || box1.max.y < box0.min.y || box0.max.y < box1.min.y);
}

// ---------------------------------------------------------------------------------------------------------------------------
bool intersection_aabb_obb(aabb box, float2 p0, float2 p1, float width)
{
    float2 dir = p1 - p0;
    float2 center = (p0 + p1) * .5f;
    float height = length(dir);
    float2 axis_j = dir / height; 
    float2 axis_i = skew(axis_j);

    // generate obb vertices
    float2 v[4];
    float2 extent_i = axis_i * width;
    float2 extent_j = axis_j * height;
    v[0] = center + extent_i + extent_j;
    v[1] = center - extent_i + extent_j;
    v[2] = center + extent_i - extent_j;
    v[3] = center - extent_i - extent_j;

    float2 obb_min = min(min3(v[0], v[1], v[2]), v[3]);
    float2 obb_max = max(max3(v[0], v[1], v[2]), v[3]);

    if (any(obb_max < box.min) || any(box.max < obb_min))
        return false;

    // generate aabb vertices
    v[0] = box.min;
    v[1] = float2(box.min.x, box.max.y);
    v[2] = float2(box.max.x, box.min.y);
    v[3] = box.max;

    // sat : aabb vertices vs obb axis
    float4 distances = float4(dot(axis_i, v[0]), dot(axis_i, v[1]), dot(axis_i, v[2]), dot(axis_i, v[3]));
    distances -= dot(center, axis_i);

    float threshold = width * .5f;
    if (all(distances > threshold) || all(distances < -threshold))
        return false;
    
    distances = float4(dot(axis_j, v[0]), dot(axis_j, v[1]), dot(axis_j, v[2]), dot(axis_j, v[3]));
    distances -= dot(center, axis_j);

    threshold = height * .5f;
    if (all(distances > threshold) || all(distances < -threshold))
        return false;

    return true;
}

// ---------------------------------------------------------------------------------------------------------------------------
bool intersection_aabb_triangle(aabb box, float2 p0, float2 p1, float2 p2)
{
    float2 p_min = min3(p0, p1, p2);
    float2 p_max = max3(p0, p1, p2);

    if (any(p_max < box.min) || any(box.max < p_min))
        return false;
    
    float2 v[4];
    v[0] = box.min;
    v[1] = box.max;
    v[2] = (float2) {box.min.x, box.max.y};
    v[3] = (float2) {box.max.x, box.min.y};

    // we can't assume any winding order for the triangle, so we check distance to the aabb's vertices sign against 
    // distance to the other vertex of the triangle sign. If all aabb's vertices have a opposite sign, it's a separate axis.
    float3 e = edge_init(p0, p1);
    float4 vertices_distance = float4(edge_distance(e, v[0]), edge_distance(e, v[1]), edge_distance(e, v[2]), edge_distance(e, v[3]));
    if (all(sign(vertices_distance) != sign(edge_distance(e, p2))))
        return false;

    e = edge_init(p1, p2);
    vertices_distance = float4(edge_distance(e, v[0]), edge_distance(e, v[1]), edge_distance(e, v[2]), edge_distance(e, v[3]));
    if (all(sign(vertices_distance) != sign(edge_distance(e, p0))))
        return false;

    e = edge_init(p2, p0);
    vertices_distance = float4(edge_distance(e, v[0]), edge_distance(e, v[1]), edge_distance(e, v[2]), edge_distance(e, v[3]));
    if (all(sign(vertices_distance) != sign(edge_distance(e, p1))))
        return false;

    return true;
}

// ---------------------------------------------------------------------------------------------------------------------------
bool intersection_aabb_pie(aabb box, float2 center, float2 direction, float2 aperture, float radius)
{
    if (!intersection_aabb_disc(box, center, radius))
        return false;

    float2 aabb_vertices[4]; 
    aabb_vertices[0] = box.min;
    aabb_vertices[1] = box.max;
    aabb_vertices[2] = float2(box.min.x, box.max.y);
    aabb_vertices[3] = float2(box.max.x, box.min.y);

    for(int i=0; i<4; ++i)
    {
        float2 center_vertex = normalize(aabb_vertices[i] - center);
        if (dot(center_vertex, direction) > aperture.y)
            return true;
    }
    return intersection_aabb_ray(box, center, direction);
}

// ---------------------------------------------------------------------------------------------------------------------------
bool intersection_aabb_arc(aabb box, float2 center, float2 direction, float2 aperture, float radius, float thickness)
{
    float half_thickness = thickness * .5f;

    if (!intersection_aabb_circle(box, center, radius, half_thickness))
        return false;

    return intersection_aabb_pie(box, center, direction, aperture, radius + half_thickness);
}

// ---------------------------------------------------------------------------------------------------------------------------
bool intersection_box_unevencapsule(aabb box, float2 p0, float2 p1, float radius0, float radius1)
{
    if (intersection_aabb_disc(box, p0, radius0))
        return true;

    if (intersection_aabb_disc(box, p1, radius1))
        return true;

    float2 direction = normalize(p1 - p0);
    float2 normal = skew(direction);

    float2 v[4];
    v[0] = p0 + normal * radius0;
    v[1] = p0 - normal * radius0;
    v[2] = p1 - normal * radius1;
    v[3] = p1 + normal * radius1;

    float2 obb_min = min(min3(v[0], v[1], v[2]), v[3]);
    float2 obb_max = max(max3(v[0], v[1], v[2]), v[3]);

    if (any(obb_max < box.min) || any(box.max < obb_min))
        return false;

    for(uint32_t i=0; i<4; ++i)
    {
        float3 edge = edge_init(v[i], v[(i+1)%4]);
        float4 vertices_distance = float4(edge_distance(edge, box.min), edge_distance(edge, float2(box.min.x, box.max.y)),
                                          edge_distance(edge, float2(box.max.x, box.min.y)), edge_distance(edge, box.max));

        if (all(vertices_distance < 0.f))
            return false;
    }

    return true;
}

// ---------------------------------------------------------------------------------------------------------------------------
bool intersection_aabb_trapezoid(aabb box, float2 p0, float2 p1, float radius0, float radius1)
{
    float2 dir = normalize(p1 - p0);
    float2 normal = skew(dir);
    
    float2 v[4];
    v[0] = p0 + normal * radius0;
    v[1] = p0 - normal * radius0;
    v[2] = p1 - normal * radius1;
    v[3] = p1 + normal * radius1;

    float2 obb_min = min(min3(v[0], v[1], v[2]), v[3]);
    float2 obb_max = max(max3(v[0], v[1], v[2]), v[3]);

    if (any(obb_max < box.min) || any(box.max < obb_min))
        return false;

    for(uint32_t i=0; i<4; ++i)
    {
        float3 edge = edge_init(v[i], v[(i+1)%4]);
        float4 vertices_distance = float4(edge_distance(edge, box.min), edge_distance(edge, float2(box.min.x, box.max.y)),
                                          edge_distance(edge, float2(box.max.x, box.min.y)), edge_distance(edge, box.max));

        if (all(vertices_distance < 0.f))
            return false;
    }
    return true;
}


// ---------------------------------------------------------------------------------------------------------------------------
bool point_in_triangle(float2 p0, float2 p1, float2 p2, float2 point)
{
    float d1, d2, d3;
    bool has_neg, has_pos;

    d1 = edge_sign(point, p0, p1);
    d2 = edge_sign(point, p1, p2);
    d3 = edge_sign(point, p2, p0);

    has_neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
    has_pos = (d1 > 0) || (d2 > 0) || (d3 > 0);

    return !(has_neg && has_pos);
}

// ---------------------------------------------------------------------------------------------------------------------------
bool point_in_ellipse(float2 p0, float2 p1, float width, float2 point)
{
    obb obox = compute_obb(p0, p1, width);
    point = obb_transform(obox, point);
    float distance =  square(point.x) / square(obox.extents.x) + square(point.y) / square(obox.extents.y);

    return (distance <= 1.f);
}

// ---------------------------------------------------------------------------------------------------------------------------
bool point_in_pie(float2 center, float2 direction, float radius, float cos_aperture, float2 point)
{
    if (distance_squared(center, point) > square(radius))
        return false;

    float2 to_point = normalize(point - center);
    return dot(to_point, direction) > cos_aperture;
}

// ---------------------------------------------------------------------------------------------------------------------------
bool intersection_ellipse_circle(float2 p0, float2 p1, float width, float2 center, float radius)
{
    obb obox = compute_obb(p0, p1, width);
    center = obb_transform(obox, center);
    
    float2 transformed_center = center / obox.extents;
    float scaled_radius = radius / min(obox.extents.x, obox.extents.y);
    float squared_distance = dot(transformed_center, transformed_center);

    return (squared_distance <= square(1.f + scaled_radius));
}

// ---------------------------------------------------------------------------------------------------------------------------
bool is_aabb_inside_ellipse(float2 p0, float2 p1, float width, aabb box)
{
    float2 aabb_vertices[4]; 
    aabb_vertices[0] = box.min;
    aabb_vertices[1] = box.max;
    aabb_vertices[2] = float2(box.min.x, box.max.y);
    aabb_vertices[3] = float2(box.max.x, box.min.y);

    obb obox = compute_obb(p0, p1, width);

    // transform each vertex in ellipse space and test all are in the ellipse
    for(int i=0; i<4; ++i)
    {
        float2 vertex_ellipse_space = obb_transform(obox, aabb_vertices[i]);
        float distance =  square(vertex_ellipse_space.x) / square(obox.extents.x) + square(vertex_ellipse_space.y) / square(obox.extents.y);
        if (distance>1.f)
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------------
bool is_aabb_inside_triangle(float2 p0, float2 p1, float2 p2, aabb box)
{
    float2 aabb_vertices[4]; 
    aabb_vertices[0] = box.min;
    aabb_vertices[1] = box.max;
    aabb_vertices[2] = float2(box.min.x, box.max.y);
    aabb_vertices[3] = float2(box.max.x, box.min.y);

    float3 edge0 = edge_init(p0, p1);
    float3 edge1 = edge_init(p1, p2);
    float3 edge2 = edge_init(p2, p0);

    for(int i=0; i<4; ++i)
    {
        float d0 = edge_distance(edge0, aabb_vertices[i]);
        float d1 = edge_distance(edge1, aabb_vertices[i]);
        float d2 = edge_distance(edge2, aabb_vertices[i]);

        bool has_neg = (d1 < 0) || (d2 < 0) || (d0 < 0);
        bool has_pos = (d1 > 0) || (d2 > 0) || (d0 > 0);

        if (has_neg&&has_pos)
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------------
bool is_aabb_inside_obb(float2 p0, float2 p1, float width, aabb box)
{
    float2 aabb_vertices[4]; 
    aabb_vertices[0] = box.min;
    aabb_vertices[1] = box.max;
    aabb_vertices[2] = float2(box.min.x, box.max.y);
    aabb_vertices[3] = float2(box.max.x, box.min.y);

    obb obox = compute_obb(p0, p1, width);

    for(int i=0; i<4; ++i)
    {
        float2 point = obb_transform(obox, aabb_vertices[i]);
        if (any(abs(point) > obox.extents))
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------------
bool is_aabb_inside_pie(float2 center, float2 direction, float2 aperture, float radius, aabb box)
{
    float2 aabb_vertices[4]; 
    aabb_vertices[0] = box.min;
    aabb_vertices[1] = box.max;
    aabb_vertices[2] = float2(box.min.x, box.max.y);
    aabb_vertices[3] = float2(box.max.x, box.min.y);

    for(int i=0; i<4; ++i)
    {
        if (!point_in_pie(center, direction, radius, aperture.y, aabb_vertices[i]))
            return false;
    }
    return true;
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

// ---------------------------------------------------------------------------------------------------------------------------
// returns true if the command intersects with the tile
// ---------------------------------------------------------------------------------------------------------------------------
bool intersection_tile_command(aabb tile_aabb, constant draw_command& cmd, constant float* data, float aa_width, float smooth_border)
{
    // grow the bounding box for anti-aliasing and smooth blend
    aabb tile_enlarge_aabb = aabb_grow(tile_aabb, (cmd.op == op_blend) ? max(aa_width, smooth_border) : aa_width);

    const bool is_hollow = (primitive_get_fillmode(cmd.type) == fill_hollow);
    command_type type = primitive_get_type(cmd.type);
    bool intersection = false;

    switch(type)
    {
        case primitive_oriented_box :
        {
            float2 p0 = float2(data[0], data[1]);
            float2 p1 = float2(data[2], data[3]);
            float width = data[4];
            aabb tile_rounded = aabb_grow(tile_enlarge_aabb, data[5]);
            intersection = intersection_aabb_obb(tile_rounded, p0, p1, width);

            if (intersection && is_hollow && is_aabb_inside_obb(p0, p1, width, tile_rounded))
                intersection = false;
            break;
        }
        case primitive_ellipse :
        {
            float2 p0 = float2(data[0], data[1]);
            float2 p1 = float2(data[2], data[3]);
            float width = data[4];
            float2 tile_center = (tile_aabb.min + tile_aabb.max) * .5f;

            aabb tile_smooth = aabb_grow(tile_enlarge_aabb, (is_hollow ? data[5] : 0.f));
            intersection = intersection_ellipse_circle(p0, p1, width, tile_center, length(aabb_get_extents(tile_smooth) * .5f));

            if (intersection && is_hollow && is_aabb_inside_ellipse(p0, p1, width, tile_smooth))
                intersection = false;
            break;
        }
        case primitive_arc :
        {
            float2 center = float2(data[0], data[1]);
            float radius = data[2];
            float2 direction = float2(data[3], data[4]);
            float2 aperture = float2(data[5], data[6]);
            float thickness = data[7];
            intersection = intersection_aabb_arc(tile_enlarge_aabb, center, direction, aperture, radius, thickness);
            break;
        }
        case primitive_pie :
        {
            float2 center = float2(data[0], data[1]);
            float radius = data[2];
            float2 direction = float2(data[3], data[4]);
            float2 aperture = float2(data[5], data[6]);

            aabb tile_smooth = aabb_grow(tile_enlarge_aabb, (is_hollow ? data[7] : 0.f));
            intersection = intersection_aabb_pie(tile_smooth, center, direction, aperture, radius);

            if (intersection && is_hollow && is_aabb_inside_pie(center, direction, aperture, radius, tile_smooth))
                intersection = false;

            break;
        }

        case primitive_disc :
        {
            float2 center = float2(data[0], data[1]);
            float radius = data[2];

            if (is_hollow)
            {
                float half_width = data[3] + max(aa_width, smooth_border);
                intersection = intersection_aabb_circle(tile_aabb, center, radius, half_width);
            }
            else
            {
                radius += max(aa_width, smooth_border);
                intersection = intersection_aabb_disc(tile_aabb, center, radius);
            }
            break;
        }
        case primitive_triangle :
        {
            float2 p0 = float2(data[0], data[1]);
            float2 p1 = float2(data[2], data[3]);
            float2 p2 = float2(data[4], data[5]);
            aabb tile_rounded = aabb_grow(tile_enlarge_aabb, data[6]);
            intersection = intersection_aabb_triangle(tile_rounded, p0, p1, p2);

            if (intersection && is_hollow && is_aabb_inside_triangle(p0, p1, p2, tile_rounded))
                intersection = false;

            break;
        }

        case primitive_uneven_capsule :
        {
            float2 p0 = float2(data[0], data[1]);
            float2 p1 = float2(data[2], data[3]);
            float radius0 = data[4];
            float radius1 = data[5];
            float2 tile_center = (tile_aabb.min + tile_aabb.max) * .5f;

            aabb tile_smooth = aabb_grow(tile_enlarge_aabb, (is_hollow ? data[6] : 0.f));

            // use sdf because the shape is not a "standard" uneven capsule
            // it preserves the tangent but it's hard to compute the bounding convex object to test against AAABB
            // so we use the bounding sphere of the tile to test instead
            intersection = sd_uneven_capsule(tile_center, p0, p1, radius0, radius1) < length(aabb_get_extents(tile_smooth) * .5f);

            break;
        }

        case primitive_trapezoid:
        {
            float2 p0 = float2(data[0], data[1]);
            float2 p1 = float2(data[2], data[3]);
            float radius0 = data[4];
            float radius1 = data[5];

            aabb tile_rounded = aabb_grow(tile_enlarge_aabb, data[6]);
            intersection = intersection_aabb_trapezoid(tile_rounded, p0, p1, radius0, radius1);
            break;
        }

        case combination_begin:
        case combination_end:
        case primitive_aabox :
        case primitive_blurred_box :
        case primitive_char : intersection = true; break;
        default : intersection = false; break;
    }

    return intersection;
}

// ---------------------------------------------------------------------------------------------------------------------------
// for each draw command, test aabb vs aabb of the region and put 1 if visible (otherwise 0)
// ---------------------------------------------------------------------------------------------------------------------------
kernel void predicate(constant draw_cmd_arguments& input [[buffer(0)]],
                      device uint8_t* predicate [[buffer(1)]],
                      uint index [[thread_position_in_grid]])
{
    if (index >= input.num_commands)
        return;

    // reverse order for the tile linked list 
    uint cmd_index = input.num_commands - index - 1;

    quantized_aabb aabb = input.commands_aabb[cmd_index];
    aabb.min_x /= REGION_SIZE; aabb.min_y /= REGION_SIZE;
    aabb.max_x /= REGION_SIZE; aabb.max_y /= REGION_SIZE;

    for(uint y=0; y<input.num_region_height; ++y)
    {
        for(uint x=0; x<input.num_region_width; ++x)
        {
            bool visible = (x >= aabb.min_x && x <= aabb.max_x && y >= aabb.min_y && y <= aabb.max_y);
            uint region_index = y * input.num_region_width + x;
            uint region_offset = region_index * input.num_commands;
            predicate[region_offset + index] = visible ? 1 : 0;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------------
// one-pass exclusive scan, using simd_prefix, threadgroup memory
// ---------------------------------------------------------------------------------------------------------------------------
kernel void exclusive_scan(constant draw_cmd_arguments& input [[buffer(0)]],
                           device const uint8_t* predicate [[buffer(1)]],
                           device uint16_t* scan [[buffer(2)]],
                           threadgroup uint16_t* simd_totals [[threadgroup(0)]],
                           threadgroup uint16_t* simd_offsets [[threadgroup(1)]],
                           uint tid_in_tg [[thread_index_in_threadgroup]],
                           uint2 tg_size [[threads_per_threadgroup]],
                           uint simd_group_id [[simdgroup_index_in_threadgroup]],
                           uint2 index [[thread_position_in_grid]])
{
    const uint threads_per_line = tg_size.x;
    const uint region_index = index.y;
    const uint region_offset = region_index * input.num_commands;
    const uint thread_index = tid_in_tg;

    // Compute where this thread starts reading/writing
    const uint thread_base_idx = thread_index * input.num_elements_per_thread;

    // Local prefix sum
    uint16_t local_sum = 0;

    for (uint i = 0; i < input.num_elements_per_thread; ++i) 
    {
        uint idx = thread_base_idx + i;
        if (idx < input.num_commands)
        {
            scan[region_offset + idx] = local_sum;
            local_sum += predicate[region_offset + idx];
        }
    }

    // Compute SIMD-group total and store
    uint16_t group_sum = simd_sum(local_sum);
    if (simd_is_first()) 
        simd_totals[simd_group_id] = group_sum;

    threadgroup_barrier(mem_flags::mem_threadgroup);

    // Prefix sum across SIMD group totals
    const uint num_simd_groups = (threads_per_line + SIMD_GROUP_SIZE - 1) / SIMD_GROUP_SIZE;
    if (thread_index < num_simd_groups) 
    {
        uint16_t v = simd_totals[thread_index];
        simd_offsets[thread_index] = simd_prefix_exclusive_sum(v);
    }

    threadgroup_barrier(mem_flags::mem_threadgroup);
    uint16_t simd_offset = simd_offsets[simd_group_id];

    // Final output write
    uint16_t thread_offset = simd_offset + simd_prefix_exclusive_sum(local_sum);
    for (uint i = 0; i < input.num_elements_per_thread; ++i) 
    {
        uint idx = thread_base_idx + i;
        if (idx < input.num_commands) 
            scan[region_offset + idx] += thread_offset;
    }
}

// ---------------------------------------------------------------------------------------------------------------------------
// bin commands for region
// ---------------------------------------------------------------------------------------------------------------------------
kernel void region_bin(constant draw_cmd_arguments& input [[buffer(0)]],
                       device uint16_t* regions_indices [[buffer(1)]],
                       device const uint16_t* scan [[buffer(2)]],
                       device const uint8_t* predicate [[buffer(3)]],
                       uint2 index [[thread_position_in_grid]])
{
    uint cmd_index = index.x;
    uint region_index = index.y;

    if (cmd_index >= input.num_commands)
        return;

    uint region_offset = region_index * input.num_commands;

    if (predicate[region_offset + cmd_index] == 1)
    {
        uint16_t position = scan[region_offset + cmd_index];
        regions_indices[region_offset + position] = input.num_commands - cmd_index - 1;
    }
}

// ---------------------------------------------------------------------------------------------------------------------------
// linked-list cleaning
//      * detect combination with no primitive and skip it
//      * we remove begin/end on combination with only one primitive
// ---------------------------------------------------------------------------------------------------------------------------
void clean_list(device tiles_data& tiles, uint16_t tile_index)
{
    uint32_t node_index = tiles.head[tile_index];
    uint32_t previous_index = INVALID_INDEX;
    uint32_t before_begin = INVALID_INDEX;
    uint32_t num_primitives = 0;

    while (node_index != INVALID_INDEX)
    {
        tile_node node = tiles.nodes[node_index];
    
        if (node.command_type == combination_begin)
        {
            before_begin = previous_index;
            previous_index = node_index;
            num_primitives = 0;
        }
        else if (node.command_type == combination_end)
        {
            // no primitive
            if (num_primitives == 0)
            {
                // change head
                if (before_begin == INVALID_INDEX)
                    tiles.head[tile_index] = node.next;
                else
                    tiles.nodes[before_begin].next = node.next;

                previous_index = before_begin;
            }
            //just one primitive, remove begin/end
            else if (num_primitives == 1)
            {
                // skip the begin command
                if (before_begin == INVALID_INDEX)
                    tiles.head[tile_index] = previous_index;
                else
                    tiles.nodes[before_begin].next = previous_index;

                // skip the end command
                tiles.nodes[previous_index].next = node.next;
            }
            else
                previous_index = node_index;
        }
        else
        {
            num_primitives++;
            previous_index = node_index;
        }

        node_index = node.next;
    }
}

// ---------------------------------------------------------------------------------------------------------------------------
// for each tile of the screen, we traverse the list of commands of the regiom and if the command has an impact on the tile
// we add the command to the linked list of the tile
// ---------------------------------------------------------------------------------------------------------------------------
kernel void tile_bin(constant draw_cmd_arguments& input [[buffer(0)]],
                     device tiles_data& output [[buffer(1)]],
                     device counters& counter [[buffer(2)]],
                     constant const uint16_t* regions_indices [[buffer(3)]],
                     ushort2 index [[thread_position_in_grid]])
{
    if (index.x >= input.num_tile_width || index.y >= input.num_tile_height)
        return;

    uint16_t tile_index = index.y * input.num_tile_width + index.x;

    // compute tile bounding box
    aabb tile_aabb = {.min = float2(index.x, index.y), .max = float2(index.x + 1, index.y + 1)};
    tile_aabb.min *= TILE_SIZE; tile_aabb.max *= TILE_SIZE;

    float smooth_border = 0.f;

    ushort2 region_pos = index / REGION_SIZE;
    uint32_t region_index = region_pos.y * input.num_region_width + region_pos.x;
    constant const uint16_t* indices = &regions_indices[region_index * input.num_commands];

    for(uint32_t i=0; i<input.num_commands; ++i)
    {
        uint32_t cmd_index = indices[i];
        if (cmd_index == LAST_COMMAND)
            break;

        constant quantized_aabb& cmd_aabb = input.commands_aabb[cmd_index];
        if (any(ushort4(index.xy, cmd_aabb.max_x, cmd_aabb.max_y) < ushort4(cmd_aabb.min_x, cmd_aabb.min_y, index.xy)))
            continue;

        constant draw_command& cmd = input.commands[cmd_index];
        constant clip_rect& clip = input.clips[cmd.clip_index];

        float2 tile_pos = float2(index * TILE_SIZE);
        if (any(tile_pos>float2(clip.max_x, clip.max_y)) || any((tile_pos + TILE_SIZE)<float2(clip.min_x, clip.min_y)))
            continue;

        command_type type = primitive_get_type(cmd.type);
        constant float* data = &input.draw_data[cmd.data_index];

        bool to_be_added = intersection_tile_command(tile_aabb, cmd, data, input.aa_width, smooth_border);

        if (type == combination_begin)
            smooth_border = 0.f;
        else if (type == combination_end)
            smooth_border = data[0];    // we traverse in reverse order, so the end comes first

        if (to_be_added)
        {
            // allocate one node
            uint new_node_index = atomic_fetch_add_explicit(&counter.num_nodes, 1, memory_order_relaxed);

            // avoid access beyond the end of the buffer
            if (new_node_index<input.max_nodes)
            {
                // insert in the linked list the new node
                output.nodes[new_node_index] = (tile_node)
                {
                    .command_index = (uint16_t)cmd_index,
                    .next  = output.head[tile_index],
                    .command_type = (uint8_t) type
                };

                output.head[tile_index] = new_node_index;
            }
        }
    }

    clean_list(output, tile_index);

    // if the tile has some draw command to proceed
    if (output.head[tile_index] != INVALID_INDEX)
    {
        uint pos = atomic_fetch_add_explicit(&counter.num_tiles, 1, memory_order_relaxed);

        // add tile index
        output.tile_indices[pos] = tile_index;
    }
}


// ---------------------------------------------------------------------------------------------------------------------------
kernel void write_icb(device counters& counter [[buffer(0)]],
                      device output_command_buffer& indirect_draw [[buffer(1)]])
{
    render_command cmd(indirect_draw.cmd_buffer, 0);
    cmd.draw_primitives(primitive_type::triangle_strip, 0, 4, atomic_load_explicit(&counter.num_tiles, memory_order_relaxed), 0);
}

