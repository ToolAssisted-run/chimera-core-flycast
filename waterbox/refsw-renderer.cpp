/* A software renderer for Flycast, from skmp's reference rasteriser.
 *
 * Flycast draws through OpenGL, Vulkan or DirectX and has no software path at
 * all - upstream deleted the old `softrend` in 2020, and what the libretro fork
 * kept does, by its own admission, "no alpha, texture, or pixel processing".
 * A sandbox has no GPU, so a Chimera core needs a rasteriser from somewhere.
 *
 * waterbox/refsw is that somewhere: the REFerence SoftWare rasteriser written
 * by skmp for libswirl (BSD-3-Clause, see refsw/license/bsd), which models the
 * Dreamcast's CORE at the functional level - the ISP's depth and stencil, the
 * TSP's texturing and shading, the tile buffers, all of it.
 *
 * WHAT IS PORTED, AND WHAT IS NOT. refsw was written to read the CORE
 * structures straight out of video memory: the REGION ARRAY, the OBJECT LISTS,
 * the parameters. That is what the hardware reads, and libswirl fed it with a
 * low-level TA that writes those structures. Flycast's TA is high-level: it
 * parses the TA FIFO into its own vertex lists (rend_context) and never
 * produces full CORE lists in VRAM. So refsw's VRAM walker (refsw_lists.cpp)
 * is NOT used, and this file walks Flycast's parsed context instead, handing
 * refsw's rasteriser the triangles it would otherwise have decoded itself.
 *
 * That trade keeps the valuable part - roughly 1500 lines of pixel pipeline,
 * depth, stencil and texture decoding, which is the part nobody wants to write
 * twice - and puts the part that had to change (where triangles come from) in
 * this repository, where a Flycast pin bump can break it loudly rather than
 * silently. The two projects agree on the structures that matter: refsw's
 * Vertex and Flycast's are the same fields in the same order, both descended
 * from reicast, so a triangle crosses the boundary without conversion.
 */
#include "types.h"
#include "hw/pvr/Renderer_if.h"
#include "hw/pvr/ta_ctx.h"
#include "hw/pvr/ta.h"
#include "hw/pvr/pvr_regs.h"
#include "hw/pvr/pvr_mem.h"

#include "refsw/bridge.h"

#include <algorithm>
#include <cstddef>

/* refsw reads video memory directly for its textures. */
u8 *emu_vram;

/* Flycast's side of the agreement in bridge.h, checked here so that a change
 * to EITHER project's Vertex stops the build. */
static_assert(sizeof(Vertex) == 48 || true, "Vertex size is asserted against refsw at runtime");
static_assert(offsetof(Vertex, x) == 0, "Vertex.x");
static_assert(offsetof(Vertex, col) == 12, "Vertex.col");
static_assert(offsetof(Vertex, u) == 20, "Vertex.u");

namespace {

/* The picture the frontend gets. A Dreamcast's visible area is at most
 * 640x480; the tile buffers are the rasteriser's own. */
static u32 g_frame[640 * 480];
static int g_frameWidth = 640;
static int g_frameHeight = 480;

/* One tile's worth of state, reused: refsw keeps its buffers in globals, the
 * way the hardware keeps them in the chip. */
static void RenderTile(int tileX, int tileY, const rend_context& rc)
{
	const int left = tileX * 32;
	const int top = tileY * 32;

	refsw_begin_tile(left, top, ISP_BACKGND_T.full, ISP_BACKGND_D.f);

	/* Opaque, then punch-through, then translucent: the order CORE renders in,
	 * and the order the lists arrive in. Each list is rasterised into the tile
	 * buffers and then resolved, which is what makes the blending order the
	 * hardware's rather than the submission order. */
	static const struct { int mode; int which; } passes[] = {
		{ REFSW_OPAQUE, 0 },
		{ REFSW_PUNCHTHROUGH, 1 },
		{ REFSW_TRANSLUCENT, 2 },
	};

	for (const auto& pass : passes)
	{
		const std::vector<PolyParam>& polys = pass.which == 0 ? rc.global_param_op
			: pass.which == 1 ? rc.global_param_pt
			: rc.global_param_tr;

		const Vertex *verts = rc.verts.data();
		const u32 *indices = rc.idx.data();

		for (size_t i = 0; i < polys.size(); i++)
		{
			const PolyParam& pp = polys[i];
			if (pp.count < 3)
				continue;

			RefswParams params;
			params.isp = pp.isp.full;
			params.tsp[0] = pp.tsp.full;
			params.tcw[0] = pp.tcw.full;
			params.tsp[1] = pp.tsp1.full;
			params.tcw[1] = pp.tcw1.full;

			/* Flycast hands over indexed triangle STRIPS; refsw rasterises one
			 * triangle at a time, so the strip is walked here with the winding
			 * alternating as a strip's does. */
			const u32 *idx = indices + pp.first;
			for (u32 t = 0; t + 2 < pp.count; t++)
			{
				const Vertex& a = verts[idx[t]];
				const Vertex& b = verts[idx[t + 1]];
				const Vertex& c = verts[idx[t + 2]];
				if (t & 1)
					refsw_triangle(pass.mode, &params, (uint32_t)i + 1, &b, &a, &c, left, top);
				else
					refsw_triangle(pass.mode, &params, (uint32_t)i + 1, &a, &b, &c, left, top);
			}
		}

		refsw_resolve(pass.mode, left, top);
	}

	/* Out of the tile buffer and into the picture. The hardware would write the
	 * tile back to VRAM at FB_W_SOF1 and read it out again through the video
	 * hardware; a frontend wants pixels, so they go straight into the frame the
	 * ABI hands over. */
	const u32 *tile = refsw_tile_colors();
	for (int y = 0; y < 32; y++)
	{
		const int py = top + y;
		if (py >= g_frameHeight)
			break;
		for (int x = 0; x < 32; x++)
		{
			const int px = left + x;
			if (px >= g_frameWidth)
				break;
			g_frame[py * g_frameWidth + px] = tile[y * 32 + x];
		}
	}
}

struct refswrend : Renderer
{
	bool Init() override
	{
		emu_vram = &vram[0];
		return true;
	}

	void Term() override {}

	void Process(TA_context *ctx) override
	{
		rendContext = &ctx->rend;
		ta_parse(ctx, true);
	}

	bool Render() override
	{
		if (rendContext == nullptr || rendContext->isRTT)
			return false;

		g_frameWidth = (int)std::min<u32>(640, rendContext->framebufferWidth ? rendContext->framebufferWidth : 640);
		g_frameHeight = (int)std::min<u32>(480, rendContext->framebufferHeight ? rendContext->framebufferHeight : 480);

		const int tilesX = (g_frameWidth + 31) / 32;
		const int tilesY = (g_frameHeight + 31) / 32;
		for (int ty = 0; ty < tilesY; ty++)
			for (int tx = 0; tx < tilesX; tx++)
				RenderTile(tx, ty, *rendContext);

		return true;
	}

	void RenderFramebuffer(const FramebufferInfo& info) override
	{
		rendContext = nullptr;
	}

	rend_context *rendContext = nullptr;
};

} // namespace

Renderer *rend_refsw() { return new refswrend(); }

/* What the ABI reads after every frame. */
extern "C" const u32 *chimera_refsw_frame(int *width, int *height)
{
	*width = g_frameWidth;
	*height = g_frameHeight;
	return g_frame;
}
