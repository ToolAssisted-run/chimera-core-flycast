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

/* Draw the translucent list in the order it is submitted, blending as it goes,
 * instead of depth peeling it. For the games whose translucency the TA sorts
 * automatically, Flycast sorts the triangles itself and every GPU backend
 * draws them this way; peeling hundreds of coplanar sprites cannot converge.
 */
void refsw_set_sorted(int on);

/* The PVR's user clip for the polygons that follow: mode 0 off, 1 draw only
 * OUTSIDE the rectangle, 2 draw only inside it. Screen pixels, half-open on
 * the right and bottom. */
void refsw_set_clip(int mode, int x0, int y0, int x1, int y1);

/* Rasterise one triangle into the current tile. `params` is a RefswParams;
 * v1..v3 are Flycast Vertex pointers, which refsw reads as its own. */
/* `key` identifies the triangle within this tile - list, polygon, position in
 * the strip - and must be the SAME every time the triangle is submitted, so
 * that its tag is stable across the peel passes. */
void refsw_triangle(int mode, const RefswParams *params, uint64_t key,
                    const void *v1, const void *v2, const void *v3,
                    int left, int top);

/* One list of a tile, drawn the way CORE draws it.
 *
 * A list is not "rasterise, then shade". CORE keeps a TAG per pixel and shades
 * once per pass, and for two of the three lists it does that REPEATEDLY:
 *
 *   opaque       one pass: whatever wins the depth test is what shows.
 *   punchthrough alpha-tested, so a pixel that fails the test leaves a hole
 *                and the layer behind it must be drawn - the list is replayed
 *                until no holes are left.
 *   translucent  depth PEELING: each pass takes the nearest layer not yet
 *                taken and blends it, and the list is replayed until every
 *                layer has been.
 *
 * That is why a pass takes a CALLBACK rather than a list of triangles: the
 * caller has to be able to submit the same triangles again, as many times as
 * the peeling needs. Calling RenderParamTags once, without the peel loops, is
 * what the first version of this file did - and it drew opaque geometry
 * correctly while dropping every punch-through and translucent polygon in
 * every game, which is a failure a test triangle cannot show.
 */
typedef void (*refsw_submit_fn)(void *user);

void refsw_pass(int mode, int left, int top, refsw_submit_fn submit, void *user);

/* Diagnostics for CHIMERA_REFSW_TRACE: triangles rasterised into this tile so
 * far, and how many of its pixels are lit. */
unsigned refsw_tile_triangles(void);
unsigned refsw_tile_lit(void);

/* The finished tile: 32x32 pixels, BGRA. */
const uint32_t *refsw_tile_colors(void);

/* Sizes the renderer checks its own structures against, so a Flycast pin bump
 * that changes a layout fails the build instead of the picture. */
extern const unsigned refsw_sizeof_vertex;
