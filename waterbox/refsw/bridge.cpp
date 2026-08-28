/* refsw's side of the seam. Includes refsw's headers and nothing of Flycast's.
 * See bridge.h for why the two never meet.
 */
/* refsw's own translation units are compiled with refsw-host.h force-included
 * (see meson.build), which is what gives the PVR registers their bitfield
 * types. This file is compiled with the driver instead, so it says which
 * registers it needs and reads them the same way. */
#include "refsw_tile.h"
#include "refsw_lists.h"
#include "refsw-host.h"
#include "bridge.h"
#include <cstdlib>

#include <cfloat>
#include <cstddef>
#include <unordered_map>
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

/* A tag has to mean the SAME triangle every time the tile sees it.
 *
 * CORE renders translucency by peeling: it draws the list again and again,
 * each pass keeping the layer nearest the one before. Where two polygons are
 * coplanar - which in a 2D game is nearly all of them - it breaks the tie by
 * comparing their TAGS, and a tag is a parameter address, so it is the same
 * number in every pass. Handing out a fresh number per submission made every
 * pass compare this pass's tags against the last pass's, which are always
 * smaller, so the tie-break dropped the pixel and the picture came apart.
 *
 * The key is where the triangle came from - list, polygon, position in the
 * strip - so the same triangle keeps its tag for as long as the tile lasts,
 * and tags still increase in submission order, which is what the tie-break
 * actually asks of them. */
static std::unordered_map<uint64_t, uint32_t> g_tagOf;

/* Diagnostics for CHIMERA_REFSW_TRACE: how many triangles this tile has taken. */
static unsigned g_triangles;

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
	g_tagOf.clear();
	g_triangles = 0;
}

void refsw_set_sorted(int on)
{
	extern int chimera_force_presort;
	chimera_force_presort = on;
}

void refsw_set_clip(int mode, int x0, int y0, int x1, int y1)
{
	extern int chimera_clip_mode, chimera_clip_x0, chimera_clip_y0,
		chimera_clip_x1, chimera_clip_y1;
	chimera_clip_mode = mode;
	chimera_clip_x0 = x0;
	chimera_clip_y0 = y0;
	chimera_clip_x1 = x1;
	chimera_clip_y1 = y1;
}

void refsw_triangle(int mode, const RefswParams *params, uint64_t key,
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

	u32 ourTag;   /* 1-based; 0 means "none" */
	auto it = g_tagOf.find(key);
	if (it != g_tagOf.end())
	{
		ourTag = it->second;
		g_registered[ourTag - 1] = reg;
	}
	else
	{
		g_registered.push_back(reg);
		ourTag = (u32)g_registered.size();
		g_tagOf.emplace(key, ourTag);
	}

	g_triangles++;
	RasterizeTriangle((RenderMode)mode, &reg.params, ourTag,
		reg.v[0], reg.v[1], reg.v[2], nullptr, &rect);
}

/* A peel loop that cannot run forever. The hardware stops when no pixel asks
 * for another layer; a sandboxed core also has to stop when something has gone
 * wrong, because a frontend cannot tell a hung guest from a slow one. Thirty
 * two layers is far past what a Dreamcast scene has and far short of a hang.
 */
static const int MAX_PEELS = 32;

void refsw_pass(int mode, int left, int top, refsw_submit_fn submit, void *user)
{
	switch ((RenderMode)mode)
	{
		case RM_OPAQUE:
			submit(user);
			RenderParamTags(RM_OPAQUE, left, top);
			break;

		case RM_PUNCHTHROUGH:
			PeelBuffersPTInitial(FLT_MAX);
			for (int peel = 0; peel < MAX_PEELS; peel++)
			{
				ClearMoreToDraw();
				submit(user);
				PeelBuffersPT();
				RenderParamTags(RM_PUNCHTHROUGH, left, top);
				PeelBuffersPTAfterHoles();
				if (!GetMoreToDraw())
					break;
			}
			break;

		case RM_TRANSLUCENT:
			ClearParamBuffer(TAG_INVALID);
			for (int peel = 0; peel < MAX_PEELS; peel++)
			{
				ClearMoreToDraw();
				extern int chimera_force_presort;
				if (!chimera_force_presort && !ISP_FEED_CFG.pre_sort)
					PeelBuffers(FLT_MAX, 0);
				submit(user);
				RenderParamTags(RM_TRANSLUCENT, left, top);
				if (!GetMoreToDraw())
					break;
			}
			break;

		default:
			break;
	}
}

/* Diagnostics: how many triangles this tile has rasterised, and how much of it
 * is lit. Both are for CHIMERA_REFSW_TRACE, which is how a "nothing is drawn"
 * report turns into a question with an answer. */
unsigned refsw_tile_triangles(void) { return g_triangles; }

unsigned refsw_tile_lit(void)
{
	unsigned lit = 0;
	for (int i = 0; i < 32 * 32; i++)
		if (colorBuffer1[i] & 0x00FFFFFF)
			lit++;
	return lit;
}

const uint32_t *refsw_tile_colors(void)
{
	return colorBuffer1;
}
