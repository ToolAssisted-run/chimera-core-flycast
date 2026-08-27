/* refsw's side of the seam. Includes refsw's headers and nothing of Flycast's.
 * See bridge.h for why the two never meet.
 */
#include "refsw_tile.h"
#include "refsw_lists.h"
#include "bridge.h"

#include <cstddef>
#include <vector>

/* The agreement, checked. If a Vertex ever stops being the same shape on both
 * sides, this is where the build stops - loudly, and before a single wrong
 * pixel is drawn. */
static_assert(sizeof(DrawParameters) == sizeof(RefswParams), "DrawParameters layout");
static_assert(offsetof(Vertex, x) == 0, "Vertex.x");
static_assert(offsetof(Vertex, col) == 12, "Vertex.col");
static_assert(offsetof(Vertex, u) == 20, "Vertex.u");

const unsigned refsw_sizeof_vertex = sizeof(Vertex);

/* ---------------------------------------------------------------------------
 * Where a triangle's parameters live.
 *
 * CORE rasterises first and shades later: each pixel keeps the TAG of whatever
 * won the depth test, and shading happens once per tag when the tile resolves.
 * On hardware a tag is a pointer into the parameter blocks in video memory,
 * which is what refsw decodes. Flycast never writes those, so every triangle
 * handed over here is remembered under its tag, and refsw's decode hook is
 * answered from this table instead.
 *
 * It is per-tile and cleared with the tile: tags are only meaningful between a
 * begin and its resolve.
 */
struct Registered {
	DrawParameters params;
	Vertex v[3];
};
static std::vector<Registered> g_registered;

extern "C" bool chimera_fpu_entry(u32 tag, taRECT *rect, void *out)
{
	const u32 index = tag & ~TAG_INVALID;
	if (index == 0 || index > g_registered.size())
		return false;

	const Registered& reg = g_registered[index - 1];
	FpuEntry *entry = (FpuEntry *)out;
	*entry = FpuEntry{};
	entry->params = reg.params;
	entry->ips.Setup(rect, &entry->params, reg.v[0], reg.v[1], reg.v[2], false);
	return true;
}

void refsw_begin_tile(int left, int top, uint32_t bgTag, float bgDepth)
{
	(void)left; (void)top;
	/* TAG_INVALID, not the CORE background tag: with no parameter blocks in
	 * VRAM there is nothing for refsw to decode a background from, so the tile
	 * starts empty and whatever is drawn covers it. The background plane comes
	 * back when it comes from Flycast's lists rather than from memory. */
	(void)bgTag;
	ClearBuffers(TAG_INVALID, bgDepth, 0);
	ClearFpuEntries();
	g_registered.clear();
}

void refsw_triangle(int mode, const RefswParams *params, uint32_t tag,
                    const void *v1, const void *v2, const void *v3,
                    int left, int top)
{
	taRECT rect;
	rect.left = left;
	rect.top = top;
	rect.right = left + 32;
	rect.bottom = top + 32;

	Registered reg;
	reg.params = *(const DrawParameters *)params;
	reg.v[0] = *(const Vertex *)v1;
	reg.v[1] = *(const Vertex *)v2;
	reg.v[2] = *(const Vertex *)v3;
	g_registered.push_back(reg);
	const u32 ourTag = (u32)g_registered.size();   /* 1-based; 0 means "none" */

	RasterizeTriangle((RenderMode)mode, &reg.params, ourTag,
		reg.v[0], reg.v[1], reg.v[2], nullptr, &rect);
}

void refsw_resolve(int mode, int tileX, int tileY)
{
	RenderParamTags((RenderMode)mode, tileX, tileY);
}

const uint32_t *refsw_tile_colors(void)
{
	return colorBuffer1;
}
