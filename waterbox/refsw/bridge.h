/* The seam between Flycast's world and refsw's.
 *
 * Both projects descend from reicast, so both define PCW, ISP_TSP, TSP, TCW
 * and Vertex - the same fields, in the same order, under the same names. That
 * is what makes feeding one from the other cheap, and it is also why their
 * headers cannot meet: a translation unit that includes both gets a redefinition
 * of every one of them.
 *
 * So they never meet. The renderer (waterbox/refsw-renderer.cpp) sees only
 * Flycast's headers, the rasteriser sees only refsw's, and triangles cross this
 * seam as raw pointers to structures both sides agree on byte for byte -
 * checked, not assumed: bridge.cpp static_asserts the sizes and offsets on
 * refsw's side, and the renderer does the same on Flycast's.
 */
#pragma once

#include <cstdint>

/* refsw's RenderMode, repeated so neither header is needed to name one */
enum RefswMode {
	REFSW_OPAQUE = 0,
	REFSW_PUNCHTHROUGH = 1,
	REFSW_OP_PT_MV = 2,
	REFSW_TRANSLUCENT = 3,
	REFSW_MODIFIER = 4,
};

/* The parameters a poly is drawn with: ISP_TSP, then TSP and TCW for each of
 * the two volumes. Laid out exactly as refsw's DrawParameters, which is how
 * the renderer can fill one without refsw's headers. */
struct RefswParams {
	uint32_t isp;
	uint32_t tsp[2];
	uint32_t tcw[2];
};

/* Start a 32x32 tile: clear its colour, depth and tag buffers the way CORE
 * clears them, from the background parameter and depth. */
void refsw_begin_tile(int left, int top, uint32_t bgTag, float bgDepth);

/* Rasterise one triangle into the current tile. `params` is a RefswParams;
 * v1..v3 are Flycast Vertex pointers, which refsw reads as its own. */
void refsw_triangle(int mode, const RefswParams *params, uint32_t tag,
                    const void *v1, const void *v2, const void *v3,
                    int left, int top);

/* Resolve what was rasterised: CORE keeps a tag per pixel and shades once per
 * list, which is what makes its blending order the hardware's. */
void refsw_resolve(int mode, int tileX, int tileY);

/* The finished tile: 32x32 pixels, BGRA. */
const uint32_t *refsw_tile_colors(void);

/* Sizes the renderer checks its own structures against, so a Flycast pin bump
 * that changes a layout fails the build instead of the picture. */
extern const unsigned refsw_sizeof_vertex;
