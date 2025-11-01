# onedraw — a GPU-driven 2D renderer

## Rasterization shaders


#### Vertex shader

The previous compute shaders produce an array of tile indices.  
An **indirect draw call** then uses this data to determine how many instances to render.  
Each instance corresponds to one tile and is drawn as a **triangle strip** with four vertices.  

In the vertex shader, the tile index is fetched from the array using the **instinstance_id**. 

```C
uint16_t tile_index = tile_indices[instance_id];
uint16_t tile_x = tile_index % input.num_tile_width;
uint16_t tile_y = tile_index / input.num_tile_width;
```

And the final vertex positions are generated based on the **vertex_id**.

```C
float2 screen_pos = float2(vertex_id&1, vertex_id>>1);
screen_pos += float2(tile_x, tile_y);
screen_pos *= TILE_SIZE;
```


We also pass the tile index to each vertex and specify **no interpolation** so the value remains constant across the entire tile.

#### Fragment shader

In the fragment shader, the **tile index** is used to retrieve the linked list of draw commands, which are then processed in order.  
The output color starts with the clear color, and standard **alpha blending** functions are applied to accumulate the final result.  

For **groups of shapes**, the selected operator (`min` or `smoothmin`) is used to accumulate distances and blend colors before contributing to the final output color.

## Signed distance functions

We mainly use the the SDF from Inigo Quilez's [website.](https://iquilezles.org/articles/distfunctions2d/)
The ellipse SDF is taken from this [shadertoy from quagnz](https://www.shadertoy.com/view/tt3yz7)
And the gaussian blurred box is inspired by [oneshade's shader](https://www.shadertoy.com/view/NsVSWy)

For the smooth minimum, we use the [quadratic polynomial](https://iquilezles.org/articles/smin/) version that also blend material attribute (in our case color).

## Anti-aliasing

Since rendering operates at the pixel level, there’s no need to compute pixel length using `ddx` or `ddy`.  
Because the alpha transition typically occurs over a very short distance (around one pixel), **linear interpolation** is used instead of `smoothstep`.  

Outlines also use the anti-aliasing technique to smooth both edges — the transition to the background and the transition to the shape’s color.




