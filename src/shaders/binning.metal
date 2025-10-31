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

static inline aabb aabb_grow(aabb box, float2 amount)
{
    return (aabb) {.min = box.min - amount, .max = box.max + amount};
}

static inline float2 aabb_get_extents(aabb box) {return box.max - box.min;}

template <class T> T square(T value) {return value*value;}

// ---------------------------------------------------------------------------------------------------------------------------
static inline float edge_distance(float2 p, float2 e0, float2 e1)
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
static inline float2 obb_transform(obb obox, float2 point)
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
static inline bool intersection_aabb_disc(aabb box, float2 center, float radius)
{
    float2 nearest_point = clamp(center.xy, box.min, box.max);
    return distance_squared(nearest_point, center) < square(radius);
}

// ---------------------------------------------------------------------------------------------------------------------------
static inline bool intersection_aabb_circle(aabb box, float2 center, float radius, float half_width)
{
    if (!intersection_aabb_disc(box, center, radius + half_width))
        return false;

    float2 candidate0 = abs(center.xy - box.min);
    float2 candidate1 = abs(center.xy - box.max);
    float2 furthest_point = max(candidate0, candidate1);

    return length_squared(furthest_point) > square(radius - half_width);
}

// ---------------------------------------------------------------------------------------------------------------------------
bool intersection_aabb_obb(aabb box, float2 p0, float2 p1, float width)
{
    float2 dir = p1 - p0;
    float2 center = (p0 + p1) * 0.5f;
    float height = length(dir);

    float2 axis_j = dir / height;
    float2 axis_i = float2(-axis_j.y, axis_j.x);

    float half_i = width * 0.5f;
    float half_j = height * 0.5f;

    float2 aabb_extent = abs(axis_i * half_i) + abs(axis_j * half_j);
    float2 obb_min = center - aabb_extent;
    float2 obb_max = center + aabb_extent;
    if (any(obb_max < box.min) || any(box.max < obb_min))
        return false;

    float2 aabb_center = (box.min + box.max) * 0.5f;
    float2 aabb_half = (box.max - box.min) * 0.5f;

    float d = abs(dot(axis_i, center - aabb_center));
    float r = aabb_half.x * abs(axis_i.x) + aabb_half.y * abs(axis_i.y);
    if (d > (half_i + r))
        return false;

    d = abs(dot(axis_j, center - aabb_center));
    r = aabb_half.x * abs(axis_j.x) + aabb_half.y * abs(axis_j.y);
    if (d > (half_j + r))
        return false;

    return true;
}


// ---------------------------------------------------------------------------------------------------------------------------
inline bool intersection_aabb_triangle(aabb box, float2 p0, float2 p1, float2 p2)
{
    float2 pmin = min(min(p0, p1), p2);
    float2 pmax = max(max(p0, p1), p2);
    if (any(pmax < box.min) || any(box.max < pmin))
        return false;

    const float2 v0 = box.min;
    const float2 v1 = box.max;
    const float2 v2 = float2(box.min.x, box.max.y);
    const float2 v3 = float2(box.max.x, box.min.y);

    #define EDGE_SEPARATION(e0, e1, refp) do {                         \
        float ref = edge_distance(refp, e0, e1);                       \
        float d0 = edge_distance(v0, e0, e1);                          \
        float d1 = edge_distance(v1, e0, e1);                          \
        float d2 = edge_distance(v2, e0, e1);                          \
        float d3 = edge_distance(v3, e0, e1);                          \
        if (ref > 0.0f) {                                              \
            if (d0 < 0.0f && d1 < 0.0f && d2 < 0.0f && d3 < 0.0f)      \
                return false;                                          \
        } else if (ref < 0.0f) {                                       \
            if (d0 > 0.0f && d1 > 0.0f && d2 > 0.0f && d3 > 0.0f)      \
                return false;                                          \
        }                                                              \
    } while (0)

    EDGE_SEPARATION(p0, p1, p2);
    EDGE_SEPARATION(p1, p2, p0);
    EDGE_SEPARATION(p2, p0, p1);

    #undef EDGE_SEPARATION

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
bool point_in_triangle(float2 p0, float2 p1, float2 p2, float2 point)
{
    float d1, d2, d3;
    bool has_neg, has_pos;

    d1 = edge_distance(point, p0, p1);
    d2 = edge_distance(point, p1, p2);
    d3 = edge_distance(point, p2, p0);

    has_neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
    has_pos = (d1 > 0) || (d2 > 0) || (d3 > 0);

    return !(has_neg && has_pos);
}

// ---------------------------------------------------------------------------------------------------------------------------
static inline bool point_in_pie(float2 center, float2 direction, float radius, float cos_aperture, float2 point)
{
    if (distance_squared(center, point) > square(radius))
        return false;

    float2 to_point = normalize(point - center);
    return dot(to_point, direction) > cos_aperture;
}

// ---------------------------------------------------------------------------------------------------------------------------
static inline bool intersection_ellipse_circle(float2 p0, float2 p1, float width, float2 center, float radius)
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

    for(int i=0; i<4; ++i)
    {
        float d0 = edge_distance(p0, p1, aabb_vertices[i]);
        float d1 = edge_distance(p1, p2, aabb_vertices[i]);
        float d2 = edge_distance(p2, p0, aabb_vertices[i]);

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

// ---------------------------------------------------------------------------------------------------------------------------
// returns true if the command intersects with the tile
// ---------------------------------------------------------------------------------------------------------------------------
bool intersection_tile_command(aabb tile_aabb, draw_command cmd, sdf_operator op, constant float* data, float aabb_margin)
{
    // grow the bounding box for anti-aliasing, smooth blend and outline
    aabb tile_enlarge_aabb = aabb_grow(tile_aabb, aabb_margin);

    const bool is_hollow = (cmd.fillmode == fill_hollow);
    bool intersection = false;

    switch(cmd.type)
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
                float half_width = data[3] + aabb_margin;
                intersection = intersection_aabb_circle(tile_aabb, center, radius, half_width);
            }
            else
            {
                radius += aabb_margin;
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
        case primitive_oriented_quad:
        {
            float2 center = float2(data[0], data[1]);
            float2 dimensions = float2(data[2], data[3]);
            float2 axis = float2(data[4], data[5]);
            float2 dir = axis * .5f/dimensions.x;
            float2 p0 = center + dir;
            float2 p1 = center - dir;
            intersection = intersection_aabb_obb(tile_aabb, p0, p1, 1.f/dimensions.y);
            break;
        }

        case begin_group:
        case end_group:
        case primitive_aabox :
        case primitive_blurred_box :
        case primitive_quad:
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
        if (position < input.num_commands)
            regions_indices[region_offset + position] = input.num_commands - cmd_index - 1;
    }
}

// ---------------------------------------------------------------------------------------------------------------------------
// linked-list cleaning
//      * detect combination with no primitive and skip it
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
    
        if (node.command_type == begin_group)
        {
            before_begin = previous_index;
            previous_index = node_index;
            num_primitives = 0;
        }
        else if (node.command_type == end_group)
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
                     ushort3 thread_pos [[thread_position_in_grid]])
{
    // index.xy = tile index relative to the region
    // index.z = region index
    ushort region_index = thread_pos.z;
    ushort2 region_xy = ushort2(region_index % input.num_region_width,
                                region_index / input.num_region_width);
    ushort2 tile_xy = region_xy * REGION_SIZE + thread_pos.xy;

    if (tile_xy.x >= input.num_tile_width || tile_xy.y >= input.num_tile_height)
        return;

    ushort tile_index = tile_xy.y * input.num_tile_width + tile_xy.x;

    // compute tile bounding box
    aabb tile_aabb = {.min = float2(tile_xy), .max = float2(tile_xy.x + 1, tile_xy.y + 1)};
    tile_aabb.min *= TILE_SIZE; tile_aabb.max *= TILE_SIZE;

    float aabb_margin = 0.f;
    sdf_operator group_op = op_overwrite;
    constant const uint16_t* indices = &regions_indices[region_index * input.num_commands];

    for(uint32_t i=0; i<input.num_commands; ++i)
    {
        uint32_t cmd_index = simd_broadcast_first(indices[i]);
        if (cmd_index == LAST_COMMAND)
            break;

        quantized_aabb cmd_aabb = input.commands_aabb[cmd_index];
        if (any(ushort4(tile_xy, cmd_aabb.max_x, cmd_aabb.max_y) < ushort4(cmd_aabb.min_x, cmd_aabb.min_y, tile_xy)))
            continue;

        draw_command cmd = input.commands[cmd_index];
        clip_rect clip = input.clips[cmd.clip_index];

        float2 tile_pos = float2(tile_xy * TILE_SIZE);
        if (any(tile_pos>float2(clip.max_x, clip.max_y)) || any((tile_pos + TILE_SIZE)<float2(clip.min_x, clip.min_y)))
            continue;

        constant float* data = &input.draw_data[cmd.data_index];

        bool to_be_added = intersection_tile_command(tile_aabb, cmd, group_op, data, input.aa_width + aabb_margin);

        // we traverse in reverse order, so the end comes first
        if (cmd.type == begin_group)
        {
            aabb_margin = 0.f;
            group_op = op_overwrite;
        }
        else if (cmd.type == end_group)
        {
            aabb_margin = data[0];
            group_op = (sdf_operator) cmd.extra;
        }

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
                    .command_type = (uint8_t) cmd.type
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

