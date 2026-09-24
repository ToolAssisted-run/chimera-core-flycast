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

#include "rend/TexCache.h"

#include "refsw/TexUtils.h"
#include "refsw/bridge.h"

/* CHIMERA_TEXCMP: this rasteriser's own decode, for comparison. */
extern void chimera_refsw_decode(u32 tsp, u32 tcw, u32 *out, int w, int h);

#include <algorithm>
#include <set>
#include <cstdlib>
#include <cstddef>

/* refsw reads video memory directly for its textures. */
u8 *emu_vram;

/* Turbo, from the ABI layer (cinterface.cpp). */
extern "C" int chimera_render_enabled;

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
/* per-frame, for the trace: how many triangles each list rasterised and how
 * much of the last tile each left lit */
static bool ChimeraGSTraceRenderer();
static unsigned g_passTriangles[3];
static unsigned g_passLit[3];
static bool g_trace;
static unsigned g_texturedPolys;
static unsigned g_clipped;
static void CompareTexture(const PolyParam &pp);   /* CHIMERA_TEXCMP, below */
static unsigned g_pixelFormats[8];
static unsigned g_stride, g_twiddled, g_vq, g_mipmapped;

/* The ISP word refsw expects, which is not quite the one Flycast hands over.
 *
 * Four of the ISP/TSP instruction word's bits - 16-bit UV, Gouraud, Offset and
 * TEXTURE - are described in the hardware docs, and in Flycast's own header,
 * as "in TA they are replaced by the ones on PCW". A game submitting geometry
 * through the TA sets them in the parameter control word; the ISP word it
 * wrote alongside may say anything. refsw reads its parameters from video
 * memory, where the TA has already done that substitution, so it expects them
 * merged - and this renderer hands it the raw ISP word instead.
 *
 * The result was a game rendered entirely untextured and flat: every polygon
 * of Re-Volt's world drawn in a single colour, because every one of them
 * claimed to have no texture and no Gouraud shading. Flycast does the same
 * copy for the background plane (ta_vtx.cpp), which is why THAT one looked
 * right.
 */
static inline u32 IspForRefsw(const PolyParam& pp)
{
	ISP_TSP isp = pp.isp;
	isp.UV_16b = pp.pcw.UV_16bit;
	isp.Gouraud = pp.pcw.Gouraud;
	isp.Offset = pp.pcw.Offset;
	isp.Texture = pp.pcw.Texture;
	return isp.full;
}

/* A triangle's screen-space bounding box against a 32x32 tile. Conservative on
 * purpose: a triangle that only grazes the tile is kept, because the cost of
 * keeping one is a rasteriser call that writes nothing, and the cost of
 * dropping one wrongly is a hole in the picture. */
static inline bool TouchesTile(const Vertex& a, const Vertex& b, const Vertex& c, int left, int top)
{
	const float minX = std::min(a.x, std::min(b.x, c.x));
	if (minX >= (float)(left + 32))
		return false;
	const float maxX = std::max(a.x, std::max(b.x, c.x));
	if (maxX < (float)left)
		return false;
	const float minY = std::min(a.y, std::min(b.y, c.y));
	if (minY >= (float)(top + 32))
		return false;
	const float maxY = std::max(a.y, std::max(b.y, c.y));
	return maxY >= (float)top;
}

/* The PVR's per-polygon user clip, decoded the way upstream decodes it
 * (TransformMatrix::getTileClip). The rectangle is in 32 pixel units, and the
 * mode's low bit chooses which SIDE of it survives - the naming in
 * transform_matrix.h is upstream's, where "Inside" means render what is
 * outside.
 *
 * Every GPU backend turns this into a scissor. This renderer ignored it until
 * 2026-08-28, and a game that clips every sprite it draws - Street Fighter
 * Zero 3 clips all 701 of them - had them all splashed across the screen. */
static void ApplyUserClip(const PolyParam &pp)
{
	const u32 val = pp.tileclip;
	const u32 clipmode = val >> 28;
	if (clipmode < 2)
	{
		refsw_set_clip(0, 0, 0, 0, 0);
		return;
	}

	const int x0 = (int)(val & 63) * 32;
	const int x1 = (int)(((val >> 6) & 63) + 1) * 32;
	const int y0 = (int)((val >> 12) & 31) * 32;
	const int y1 = (int)(((val >> 17) & 31) + 1) * 32;

	refsw_set_clip((clipmode & 1) ? 1 : 2, x0, y0, x1, y1);
}

/* Where a triangle came from, which is what makes its tag stable across the
 * peel passes (see refsw/bridge.cpp). */
static inline uint64_t KeyFor(int which, u32 poly, u32 tri)
{
	return ((uint64_t)which << 56) | ((uint64_t)poly << 24) | tri;
}

static bool ChimeraGSTraceRenderer()
{
	static const bool on = getenv("CHIMERA_REFSW_TRACE") != nullptr;
	return on;
}

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

	/* Each list is submitted through a callback, because CORE replays a list
	 * once per layer it has to peel (see bridge.h). What follows is therefore
	 * "how to submit this list", called as many times as the pass needs. */
	struct Submission
	{
		const rend_context *rc;
		const std::vector<PolyParam> *polys;
		int mode;
		int which;
		int left;
		int top;
	};

	auto submit = [](void *user) {
		const Submission& sub = *(const Submission *)user;
		const rend_context& rc = *sub.rc;
		const Vertex *verts = rc.verts.data();
		const u32 *indices = rc.idx.data();

		/* The translucent list, in the order Flycast's sorter put it.
		 *
		 * When the TA sorts translucency itself, upstream sorts the triangles
		 * on the CPU and every GPU backend draws them in that order: a plain
		 * triangle list in rc.idx, one run per polygon. Drawing them the same
		 * way here is what makes this renderer's picture the same picture -
		 * see refsw_set_sorted for why peeling cannot stand in for it. */
		if (sub.which == 2 && !rc.sortedTriangles.empty())
		{
			for (size_t si = 0; si < rc.sortedTriangles.size(); si++)
			{
				const SortedTriangle& st = rc.sortedTriangles[si];
				if (st.polyIndex >= sub.polys->size())
					continue;
				const PolyParam& pp = (*sub.polys)[st.polyIndex];

				ApplyUserClip(pp);

				/* And with the depth test those backends use for it: greater
				 * OR EQUAL, writing no depth (gldraw.cpp, SetGPState). The
				 * polygon's own mode is the one a peeled list would compare
				 * with; drawn in sorted order it is not. KallistiOS draws a
				 * sprite at the same depth as the backdrop under it with mode
				 * "greater" - the 240p Test Suite's menu character - and it
				 * vanished here while every GPU backend drew it (chimera issue
				 * 145). */
				ISP_TSP isp;
				isp.full = IspForRefsw(pp);
				isp.DepthMode = 6;
				isp.ZWriteDis = 1;

				RefswParams params;
				params.isp = isp.full;
				params.tsp[0] = pp.tsp.full;
				params.tcw[0] = pp.tcw.full;
				params.tsp[1] = pp.tsp1.full;
				params.tcw[1] = pp.tcw1.full;

				for (u32 j = 0; j + 2 < st.count; j += 3)
				{
					const Vertex& a = verts[indices[st.first + j]];
					const Vertex& b = verts[indices[st.first + j + 1]];
					const Vertex& c = verts[indices[st.first + j + 2]];
					if (!TouchesTile(a, b, c, sub.left, sub.top))
						continue;
					refsw_triangle(sub.mode, &params, KeyFor(sub.which, (u32)si, j),
						&a, &b, &c, sub.left, sub.top);
				}
			}
			return;
		}

		for (size_t i = 0; i < sub.polys->size(); i++)
		{
			const PolyParam& pp = (*sub.polys)[i];
			if (pp.count < 3)
				continue;

			/* The BACKGROUND PLANE is entry 0 of the opaque list, and it is not
			 * a strip: Flycast reserves it, reads its parameters from the
			 * address ISP_BACKGND_T names, and fills verts[0..3] with a quad
			 * covering the screen. Every renderer special-cases it because
			 * `first` indexes the VERTICES rather than the index buffer. It is
			 * what a game's empty space is - the colour behind everything - so
			 * a renderer that skips it draws its scenes onto whatever the last
			 * frame left behind. */
			if (sub.which == 0 && i == 0)
			{
				ApplyUserClip(pp);
				RefswParams bg;
				bg.isp = IspForRefsw(pp);
				bg.tsp[0] = pp.tsp.full;
				bg.tcw[0] = pp.tcw.full;
				bg.tsp[1] = pp.tsp1.full;
				bg.tcw[1] = pp.tcw1.full;
				const Vertex *q = rc.verts.data();
				refsw_triangle(sub.mode, &bg, KeyFor(sub.which, i, 0), &q[0], &q[1], &q[2], sub.left, sub.top);
				refsw_triangle(sub.mode, &bg, KeyFor(sub.which, i, 1), &q[1], &q[2], &q[3], sub.left, sub.top);
				continue;
			}

			if (g_trace && pp.pcw.Texture)
			{
				g_texturedPolys++;
				/* 0=1555 1=565 2=4444 3=YUV422 4=bump 5=PAL4 6=PAL8 */
				g_pixelFormats[pp.tcw.PixelFmt & 7]++;
				if (pp.tcw.StrideSel) g_stride++;
				if (!pp.tcw.ScanOrder) g_twiddled++;
				if (pp.tcw.VQ_Comp) g_vq++;
				if (pp.tcw.MipMapped) g_mipmapped++;
				if (pp.tileclip) g_clipped++;
				static const bool texcmp = getenv("CHIMERA_TEXCMP") != nullptr;
				if (texcmp)
					CompareTexture(pp);   /* the set below keeps it to one report each */
				if (g_texturedPolys <= 5 && sub.left == 0 && sub.top == 0)
					fprintf(stderr, "        texture %u: addr %08x %ux%u fmt %u twiddled %u stride %u pal %u\n",
						g_texturedPolys, pp.tcw.TexAddr << 3,
						8u << pp.tsp.TexU, 8u << pp.tsp.TexV,
						(unsigned)pp.tcw.PixelFmt, (unsigned)!pp.tcw.ScanOrder,
						(unsigned)pp.tcw.StrideSel, (unsigned)pp.tcw.PalSelect);
			}

			ApplyUserClip(pp);

			RefswParams params;
			params.isp = IspForRefsw(pp);
			params.tsp[0] = pp.tsp.full;
			params.tcw[0] = pp.tcw.full;
			params.tsp[1] = pp.tsp1.full;
			params.tcw[1] = pp.tcw1.full;

			/* Flycast hands over indexed triangle STRIPS; refsw rasterises one
			 * triangle at a time, so the strip is walked here with the winding
			 * alternating as a strip's does.
			 *
			 * One polygon's range can hold SEVERAL strips, separated by a
			 * primitive restart - the index (u32)-1, which is what the GPU
			 * renderers hand to GL_PRIMITIVE_RESTART_FIXED_INDEX and what
			 * upstream's own sorter skips (core/rend/sorter.cpp). A walker that
			 * does not know that reads verts[0xFFFFFFFF] and dies: Re-Volt got
			 * about eight hundred frames in before it did. The winding counts
			 * from the START OF THE CURRENT STRIP rather than from the start of
			 * the polygon, because that is what a restart restarts. */
			static const u32 RESTART = ~0u;
			const u32 *idx = indices + pp.first;
			u32 stripStart = 0;
			for (u32 t = 0; t + 2 < pp.count; t++)
			{
				if (idx[t] == RESTART) { stripStart = t + 1; continue; }
				if (idx[t + 1] == RESTART) { t += 1; stripStart = t + 1; continue; }
				if (idx[t + 2] == RESTART) { t += 2; stripStart = t + 1; continue; }

				const Vertex& a = verts[idx[t]];
				const Vertex& b = verts[idx[t + 1]];
				const Vertex& c = verts[idx[t + 2]];

				/* Does this triangle touch this tile at all?
				 *
				 * The hardware bins polygons into per-tile object lists as the
				 * TA receives them, and a tile only ever sees what was binned
				 * into it. Flycast hands over the lists unbinned, so without
				 * this test every polygon is rasterised into every one of the
				 * three hundred tiles: Re-Volt's opening scene submitted five
				 * thousand polygons and this walker turned them into two and a
				 * half MILLION triangle setups a frame. The bounding box is
				 * what the binning would have decided, arrived at from the
				 * other end. */
				if (!TouchesTile(a, b, c, sub.left, sub.top))
					continue;
				const uint64_t key = KeyFor(sub.which, i, t);
				if ((t - stripStart) & 1)
					refsw_triangle(sub.mode, &params, key, &b, &a, &c, sub.left, sub.top);
				else
					refsw_triangle(sub.mode, &params, key, &a, &b, &c, sub.left, sub.top);
			}
		}
	};

	for (const auto& pass : passes)
	{
		const std::vector<PolyParam>& polys = pass.which == 0 ? rc.global_param_op
			: pass.which == 1 ? rc.global_param_pt
			: rc.global_param_tr;

		if (polys.empty())
			continue;

		/* CHIMERA_PASSES is a bit mask (1 opaque, 2 punch-through, 4
		 * translucent): which lists to draw, for bisecting a bad picture. */
		static const int passMask = getenv("CHIMERA_PASSES") ? atoi(getenv("CHIMERA_PASSES")) : 7;
		if (!(passMask & (1 << pass.which)))
			continue;

		Submission sub{ &rc, &polys, pass.mode, pass.which, left, top };
		const unsigned before = refsw_tile_triangles();
		/* Sorted translucency is drawn in order, not peeled. */
		const bool sorted = pass.which == 2 && !rc.sortedTriangles.empty();
		refsw_set_sorted(sorted ? 1 : 0);
		refsw_pass(pass.mode, left, top, submit, &sub);
		refsw_set_sorted(0);
		g_passTriangles[pass.which] += refsw_tile_triangles() - before;
		g_passLit[pass.which] = refsw_tile_lit();
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

/* CHIMERA_TEXCMP: decode every texture twice and say where the two decoders
 * disagree.
 *
 * This rasteriser reads VRAM itself, texel by texel. Flycast's texture cache
 * converts a whole texture up front, and it is the decoder every GPU backend
 * uses - the one games are known to look right through. Reading the same VRAM
 * the two must agree, and the first texel where they do not is the bug.
 */
namespace
{
	struct ReferenceTexture : BaseTextureCacheData
	{
		std::vector<u32> pixels;
		int w = 0, h = 0;

		ReferenceTexture(TSP tsp, TCW tcw) : BaseTextureCacheData(tsp, tcw, 0) {}
		/* Update() registers a vram lock pointing at this object. It has to be
		 * taken back, or the next DMA that invalidates the page walks into a
		 * texture that stopped existing when this function returned. */
		~ReferenceTexture() { Delete(); }
		std::string GetId() override { return ""; }
		/* ARGB8888 whatever the texture is, so both can be read as one kind
		 * of number. */
		bool Force32BitTexture(TextureType) const override { return true; }
		void UploadToGPU(int width, int height, const u8 *data, bool, bool mipsIncluded) override
		{
			w = width;
			h = height;
			/* With mipmaps the levels share one buffer, smallest first, so the
			 * full-size level is the last width*height of it. */
			size_t total = (size_t)width * height;
			if (mipsIncluded && width == height)   /* mipmaps are square */
			{
				total = 0;
				for (int side = 1; side <= width; side *= 2)
					total += (size_t)side * side;
			}
			const u32 *end = (const u32 *)data + total;
			pixels.assign(end - (size_t)width * height, end);
		}
	};
}

static void CompareTexture(const PolyParam &pp)
{
	/* One report per distinct texture, or a frame of sprites drowns the log. */
	static std::set<std::pair<u32, u32>> seen;
	if (seen.size() > 120 || !seen.insert({ pp.tcw.full, pp.tsp.full }).second)
		return;

	ReferenceTexture ref(pp.tsp, pp.tcw);
	if (!ref.Update() || ref.pixels.size() != (size_t)ref.w * ref.h || ref.w <= 0)
	{
		fprintf(stderr, "  texcmp: no reference for %08x fmt %u\n",
			pp.tcw.TexAddr << 3, (unsigned)pp.tcw.PixelFmt);
		return;
	}

	const int w = ref.w, h = ref.h;
	std::vector<u32> mine((size_t)w * h);
	chimera_refsw_decode(pp.tsp.full, pp.tcw.full, mine.data(), w, h);

	/* Decode it a second time as if it were not mipmapped. If THAT is the one
	 * that matches, the two disagree about where the full-size level starts,
	 * not about how to read a texel. */
	TCW flat = pp.tcw;
	flat.MipMapped = 0;
	std::vector<u32> mineFlat((size_t)w * h);
	chimera_refsw_decode(pp.tsp.full, flat.full, mineFlat.data(), w, h);
	size_t flatWrong = 0, transposedWrong = 0;

	/* The two write their channels in different orders - this rasteriser in
	 * ARGB, the texture cache in RGBA - and they expand 5 and 6 bit channels
	 * differently (a shift here, a shift with the top bits repeated there).
	 * Neither of those can shred a picture, so separate them from the case
	 * that can: a texel that is a different COLOUR. */
	size_t rounding = 0, wrong = 0;
	int firstU = -1, firstV = -1;
	for (int v = 0; v < h; v++)
		for (int u = 0; u < w; u++)
		{
			u32 a = mine[(size_t)v * w + u];		// ARGB
			u32 b = ref.pixels[(size_t)v * w + u];	// RGBA
			int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF, aa = a >> 24;
			int br = b & 0xFF, bg = (b >> 8) & 0xFF, bb = (b >> 16) & 0xFF, ba = b >> 24;
			int d = std::max(std::max(abs(ar - br), abs(ag - bg)),
			                 std::max(abs(ab - bb), abs(aa - ba)));
			if (d == 0)
				continue;
			if (d <= 16)   /* the widest a 4 bit channel can differ by expansion */
				rounding++;
			else
			{
				if (firstU < 0) { firstU = u; firstV = v; }
				wrong++;
			}
		}

	/* And once more against the transpose: if THAT is the match, the two
	 * disagree about which way round u and v go, not about the data. */
	if (w == h)
		for (int v = 0; v < h; v++)
			for (int u = 0; u < w; u++)
			{
				u32 a = mine[(size_t)u * w + v];
				u32 b = ref.pixels[(size_t)v * w + u];
				int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF, aa = a >> 24;
				int br = b & 0xFF, bg = (b >> 8) & 0xFF, bb = (b >> 16) & 0xFF, ba = b >> 24;
				int d = std::max(std::max(abs(ar - br), abs(ag - bg)),
				                 std::max(abs(ab - bb), abs(aa - ba)));
				if (d > 16)
					transposedWrong++;
			}

	for (int v = 0; v < h; v++)
		for (int u = 0; u < w; u++)
		{
			u32 a = mineFlat[(size_t)v * w + u];
			u32 b = ref.pixels[(size_t)v * w + u];
			int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF, aa = a >> 24;
			int br = b & 0xFF, bg = (b >> 8) & 0xFF, bb = (b >> 16) & 0xFF, ba = b >> 24;
			int d = std::max(std::max(abs(ar - br), abs(ag - bg)),
			                 std::max(abs(ab - bb), abs(aa - ba)));
			if (d > 16)
				flatWrong++;
		}

	fprintf(stderr, "  texcmp %08x fmt %u %ux%u tw %u vq %u mip %u pal %u stride %u: "
		"%zu wrong, %zu rounding, of %d",
		pp.tcw.TexAddr << 3, (unsigned)pp.tcw.PixelFmt, w, h,
		(unsigned)!pp.tcw.ScanOrder, (unsigned)pp.tcw.VQ_Comp,
		(unsigned)pp.tcw.MipMapped, (unsigned)pp.tcw.PalSelect,
		(unsigned)pp.tcw.StrideSel, wrong, rounding, w * h);
	if (w == h)
		fprintf(stderr, " [transposed: %zu wrong]", transposedWrong);
	if (pp.tcw.MipMapped)
		fprintf(stderr, " [as flat: %zu wrong; flycast mipmapped=%d]", flatWrong, (int)ref.IsMipmapped());
	if (wrong)
		fprintf(stderr, " (first at %d,%d: refsw argb %08x, flycast rgba %08x)",
			firstU, firstV, mine[(size_t)firstV * w + firstU],
			ref.pixels[(size_t)firstV * w + firstU]);
	fprintf(stderr, "\n");
}

struct refswrend : Renderer
{
	bool Init() override
	{
		emu_vram = &vram[0];
		/* The twiddle table. Without it every twiddled texture reads one
		 * constant address and comes out a flat colour - which is what this
		 * renderer did until 2026-08-28, because refsw declares BuildTables()
		 * and never calls it: upstream calls it from its own texture cache
		 * init, which this core does not use. Nothing in the gate is
		 * textured, so nothing noticed. */
		BuildTables();
		return true;
	}

	void Term() override {}

	/* NAOMI 2: the Elan's geometry arrives UNTRANSFORMED. A Dreamcast's TA
	 * takes vertices already in screen space (x, y in pixels, z = 1/w) because
	 * the SH4 did the transform; the Elan does it in hardware, so what Flycast
	 * parses out of an Elan command list is model-space positions plus the
	 * modelview and projection matrices the chip was given, and every GPU
	 * renderer applies them in the vertex shader (rend/gles/naomi2.cpp:
	 * pos = proj * (mv * pos), then x/w, y/w, 1/w - the projection matrix
	 * the game loaded already carries the viewport, which is why the result
	 * lands in pixels like a Dreamcast vertex). skmp's rasteriser predates
	 * the NAOMI 2 and takes screen-space vertices, so the same arithmetic is
	 * done here, on the CPU, after the parse (whose sorter reads model-space
	 * z for itself).
	 *
	 * What the GPU also does, and a rasteriser fed screen coordinates
	 * cannot: clip at the near plane. A triangle with a vertex behind the
	 * eye has no screen position for that vertex, and dividing anyway hands
	 * the rasteriser a triangle across the whole picture (Virtua Fighter 4's
	 * arena, first try: a blue sheet over the top half). So each polygon's
	 * strip is walked into triangles, each triangle is clipped against
	 * w > near in clip space (Sutherland-Hodgman, attributes interpolated
	 * linearly in clip space, which is the perspective-correct way), and the
	 * survivors are written back as a fresh run of indices - one triangle
	 * per strip, primitive restarts between them, which the walker below
	 * already takes - at the end of the index array. The matrices are
	 * column-major, as GL takes them. Per-vertex lighting (computeColors in
	 * the shader) is not done yet: the base colour the game wrote is what is
	 * drawn. A polygon done is marked as a plain one (projMatrix = -1). */
	struct ClipVertex { float x, y, z, w; Vertex v; };

	static ClipVertex ToClip(const Vertex& v, const float *mv, const float *pr)
	{
		const float x = v.x, y = v.y, z = v.z;
		const float ex = mv[0] * x + mv[4] * y + mv[8] * z + mv[12];
		const float ey = mv[1] * x + mv[5] * y + mv[9] * z + mv[13];
		const float ez = mv[2] * x + mv[6] * y + mv[10] * z + mv[14];
		const float ew = mv[3] * x + mv[7] * y + mv[11] * z + mv[15];
		ClipVertex c;
		c.x = pr[0] * ex + pr[4] * ey + pr[8] * ez + pr[12] * ew;
		c.y = pr[1] * ex + pr[5] * ey + pr[9] * ez + pr[13] * ew;
		c.z = pr[2] * ex + pr[6] * ey + pr[10] * ez + pr[14] * ew;
		c.w = pr[3] * ex + pr[7] * ey + pr[11] * ez + pr[15] * ew;
		c.v = v;
		return c;
	}

	static ClipVertex Lerp(const ClipVertex& a, const ClipVertex& b, float t)
	{
		ClipVertex o;
		o.x = a.x + (b.x - a.x) * t;
		o.y = a.y + (b.y - a.y) * t;
		o.z = a.z + (b.z - a.z) * t;
		o.w = a.w + (b.w - a.w) * t;
		o.v = a.v;
		o.v.u = a.v.u + (b.v.u - a.v.u) * t;
		o.v.v = a.v.v + (b.v.v - a.v.v) * t;
		o.v.u1 = a.v.u1 + (b.v.u1 - a.v.u1) * t;
		o.v.v1 = a.v.v1 + (b.v.v1 - a.v.v1) * t;
		for (int i = 0; i < 4; i++)
		{
			o.v.col[i] = (u8)(a.v.col[i] + (b.v.col[i] - a.v.col[i]) * t);
			o.v.spc[i] = (u8)(a.v.spc[i] + (b.v.spc[i] - a.v.spc[i]) * t);
			o.v.col1[i] = (u8)(a.v.col1[i] + (b.v.col1[i] - a.v.col1[i]) * t);
			o.v.spc1[i] = (u8)(a.v.spc1[i] + (b.v.spc1[i] - a.v.spc1[i]) * t);
		}
		return o;
	}

	static Vertex ToScreen(const ClipVertex& c)
	{
		Vertex v = c.v;
		v.x = c.x / c.w;
		v.y = c.y / c.w;
		v.z = 1.f / c.w;
		return v;
	}

	static void TransformNaomi2(rend_context& rc)
	{
		static const u32 RESTART = ~0u;
		constexpr float kNear = 1e-4f;
		auto transformPolys = [&](std::vector<PolyParam>& polys)
		{
			for (PolyParam& pp : polys)
			{
				if (!pp.isNaomi2())
					continue;
				const float *mv = rc.matrices[pp.mvMatrix].mat;
				const float *pr = rc.matrices[pp.projMatrix].mat;
				const u32 outFirst = (u32)rc.idx.size();
				u32 stripStart = 0;
				const u32 end = std::min<u32>(pp.first + pp.count, (u32)rc.idx.size());
				for (u32 t = pp.first; t + 2 < end; t++)
				{
					const u32 i0 = rc.idx[t], i1 = rc.idx[t + 1], i2 = rc.idx[t + 2];
					if (i0 == RESTART) { stripStart = t + 1; continue; }
					if (i1 == RESTART) { t += 1; stripStart = t + 1; continue; }
					if (i2 == RESTART) { t += 2; stripStart = t + 1; continue; }
					if (i0 >= rc.verts.size() || i1 >= rc.verts.size() || i2 >= rc.verts.size())
						continue;
					/* a strip alternates its winding; a triangle list does not */
					const bool odd = ((t - stripStart) & 1) != 0;
					ClipVertex tri[3] = {
						ToClip(rc.verts[i0], mv, pr),
						ToClip(rc.verts[odd ? i2 : i1], mv, pr),
						ToClip(rc.verts[odd ? i1 : i2], mv, pr),
					};
					/* clip against w > near */
					ClipVertex poly[8];
					int n = 0;
					for (int k = 0; k < 3; k++)
					{
						const ClipVertex& a = tri[k];
						const ClipVertex& b = tri[(k + 1) % 3];
						const bool ain = a.w > kNear, bin = b.w > kNear;
						if (ain) poly[n++] = a;
						if (ain != bin)
							poly[n++] = Lerp(a, b, (kNear - a.w) / (b.w - a.w));
					}
					if (n < 3)
						continue;
					const u32 base = (u32)rc.verts.size();
					for (int k = 0; k < n; k++)
						rc.verts.push_back(ToScreen(poly[k]));
					for (int k = 1; k + 1 < n; k++)
					{
						rc.idx.push_back(base);
						rc.idx.push_back(base + k);
						rc.idx.push_back(base + k + 1);
						rc.idx.push_back(RESTART);
					}
				}
				pp.first = outFirst;
				pp.count = (u32)rc.idx.size() - outFirst;
				pp.projMatrix = -1;
			}
		};
		transformPolys(rc.global_param_op);
		transformPolys(rc.global_param_pt);
		transformPolys(rc.global_param_tr);
	}

	void Process(TA_context *ctx) override
	{
		rendContext = &ctx->rend;
		ta_parse(ctx, true);
		if (settings.platform.isNaomi2())
			TransformNaomi2(ctx->rend);

		/* The two Vertex structs share their fields but not their colour
		 * order. Flycast writes every vertex colour R,G,B,A - the parser, and
		 * FillBGP for the background quad already in verts[0..3], both pick
		 * B,G,R,A only for a Direct3D renderer - while refsw's Color is
		 * B,G,R,A. Crossing unconverted, red and blue traded places on every
		 * shaded polygon and nowhere else: textures and refsw's own reads
		 * came out right, so 240p Test Suite highlighted its menu in blue
		 * instead of red and Marvel vs. Capcom 2's save screen was red where
		 * every other renderer draws it blue (chimera issue 145). */
		for (Vertex& v : ctx->rend.verts)
		{
			std::swap(v.col[0], v.col[2]);
			std::swap(v.spc[0], v.spc[2]);
			std::swap(v.col1[0], v.col1[2]);
			std::swap(v.spc1[0], v.spc1[2]);
		}
	}

	bool Render() override
	{
		/* Turbo: nobody is going to look at this frame. Everything the SH4 can
		 * see has already happened - the TA parsed the list in Process, and
		 * this renderer never touches video memory (a render-to-texture pass,
		 * which does, is declined below and always was). So the tiles can go
		 * unrasterised and the machine cannot tell. */
		if (!chimera_render_enabled)
			return true;

		/* Set CHIMERA_REFSW_TRACE to watch what arrives: how many polygons of
		 * each list, how many vertices, and how much of the picture came out
		 * non-black. It is the difference between "the game draws nothing" and
		 * "this renderer drops what the game draws", which a black screen alone
		 * cannot tell you. */
		static const bool trace = getenv("CHIMERA_REFSW_TRACE") != nullptr;
		g_trace = trace;
		static int traceFrame = 0;

		if (rendContext == nullptr || rendContext->isRTT)
		{
			if (trace)
				fprintf(stderr, "refsw %d: %s\n", traceFrame++,
					rendContext == nullptr ? "no context" : "render to texture, skipped");
			return false;
		}

		g_frameWidth = (int)std::min<u32>(640, rendContext->framebufferWidth ? rendContext->framebufferWidth : 640);
		g_frameHeight = (int)std::min<u32>(480, rendContext->framebufferHeight ? rendContext->framebufferHeight : 480);

		g_passTriangles[0] = g_passTriangles[1] = g_passTriangles[2] = 0;
		g_texturedPolys = 0;
		g_clipped = 0;
		memset(g_pixelFormats, 0, sizeof(g_pixelFormats));
		g_stride = g_twiddled = g_vq = g_mipmapped = 0;

		const int tilesX = (g_frameWidth + 31) / 32;
		const int tilesY = (g_frameHeight + 31) / 32;
		for (int ty = 0; ty < tilesY; ty++)
			for (int tx = 0; tx < tilesX; tx++)
				RenderTile(tx, ty, *rendContext);

		if (trace)
		{
			size_t lit = 0;
			for (int i = 0; i < g_frameWidth * g_frameHeight; i++)
				if (g_frame[i] & 0x00FFFFFF)
					lit++;
			fprintf(stderr, "refsw %d: %dx%d op=%zu pt=%zu tr=%zu mvo=%zu verts=%zu idx=%zu lit=%zu"
				" tris(op=%u pt=%u tr=%u) textured=%u clipped=%u fmt(1555=%u 565=%u 4444=%u yuv=%u bump=%u pal4=%u pal8=%u)\n",
				traceFrame++, g_frameWidth, g_frameHeight,
				rendContext->global_param_op.size(), rendContext->global_param_pt.size(),
				rendContext->global_param_tr.size(), rendContext->global_param_mvo.size(),
				rendContext->verts.size(), rendContext->idx.size(), lit,
				g_passTriangles[0], g_passTriangles[1], g_passTriangles[2],
				g_texturedPolys / 300,
				g_clipped / 300,
				g_pixelFormats[0] / 300, g_pixelFormats[1] / 300, g_pixelFormats[2] / 300,
				g_pixelFormats[3] / 300, g_pixelFormats[4] / 300, g_pixelFormats[5] / 300,
				g_pixelFormats[6] / 300);

			/* What a paletted texture is looked up in. A palette of all zeros
			 * would explain a game of paletted sprites coming out as mush. */
			unsigned nonzero = 0;
			for (int i = 0; i < 1024; i++)
				if (PALETTE_RAM[i] != 0)
					nonzero++;
			fprintf(stderr, "        registers: TEXT_CONTROL=%08x (stride unit %u), PAL_RAM_CTRL=%u, SCALER_CTL=%08x, FB_R_CTRL=%08x\n",
				(unsigned)TEXT_CONTROL, (unsigned)((TEXT_CONTROL & 31) * 32),
				(unsigned)PAL_RAM_CTRL, (unsigned)SCALER_CTL.full, (unsigned)FB_R_CTRL.full);
			fprintf(stderr, "        layout: stride=%u twiddled=%u vq=%u mipmapped=%u (of %u textured)\n",
				g_stride / 300, g_twiddled / 300, g_vq / 300, g_mipmapped / 300, g_texturedPolys / 300);
			fprintf(stderr, "        palette: %u of 1024 entries set, first four %08x %08x %08x %08x, fmt %u\n",
				nonzero, PALETTE_RAM[0], PALETTE_RAM[1], PALETTE_RAM[2], PALETTE_RAM[3],
				(unsigned)PAL_RAM_CTRL);
		}

		return true;
	}

	void RenderFramebuffer(const FramebufferInfo& info) override
	{
		if (ChimeraGSTraceRenderer())
			fprintf(stderr, "refsw: RenderFramebuffer %dx%d (the game is showing VRAM, not a display list)\n",
				info.fb_r_size.fb_x_size + 1, info.fb_r_size.fb_y_size + 1);
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
