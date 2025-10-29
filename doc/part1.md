# onedraw — a GPU-driven 2D renderer

## Binning commands

Binning is the process that generates the **per-tile linked lists** of draw commands.  
It works by performing intersection tests between each draw command and the bounding box of every tile.  
Classic intersection methods are used, such as the **Separating Axis Theorem (SAT)** for oriented shapes and **distance-to-center** checks for circular ones.

Some additional factors are also considered:
* anti-aliasing  
* groups of shapes  
* smoothmin  

---

## Anti-aliasing

The width of the anti-aliasing region (defined by the user in onedraw) must be taken into account during intersection testing.

![anti-aliasing](anti-aliasing.png)

In the example above, even if the disc does not mathematically intersect the tile’s bounding box, the anti-aliasing width extends beyond it.  
To avoid visible seams or straight edges along tile borders, the disc is still added to the tile’s linked list so that edge pixels are correctly shaded.

## Group of shapes
