/* What refsw expects of its host, answered for Flycast.
 *
 * refsw is skmp's reference software rasteriser, written for libswirl (BSD-3,
 * see license/bsd). It models the Dreamcast's CORE at the functional level:
 * it walks the REGION ARRAY in video memory, rasterises 32x32 tiles, and
 * writes them back out to video memory - exactly as the hardware does, and
 * exactly what a sandbox needs, because it asks for no GPU at all.
 *
 * The vendored files are UNMODIFIED. Everything they need that libswirl spelled
 * differently is spelled here instead, which keeps "what did we change to make
 * this work" answerable: nothing, so far.
 */
#pragma once

#include "types.h"

/* libswirl passes the VRAM base to every access; Flycast keeps it in a
 * RamRegion and its vri/vrf take an address alone. */
extern u8 *emu_vram;

static inline u32 vri(u8 *vram, u32 addr) { return *(u32 *)&vram[addr & VRAM_MASK]; }
static inline f32 vrf(u8 *vram, u32 addr) { return *(f32 *)&vram[addr & VRAM_MASK]; }
static inline u16 vrs(u8 *vram, u32 addr) { return *(u16 *)&vram[addr & VRAM_MASK]; }

/* ---------------------------------------------------------------------------
 * PVR registers, with their fields.
 *
 * Both projects keep the register file as raw bytes and overlay a type on it;
 * they differ in WHICH registers got a structured type. libswirl gave these
 * three theirs, Flycast leaves them as plain u32, and refsw reads them by
 * field. The layouts below are libswirl's, and they describe the same hardware
 * bits either way - so the overlay is put back here rather than either project
 * being changed.
 */
#include "hw/pvr/pvr_regs.h"

union ISP_FEED_CFG_type {
	struct {
		u32 pre_sort : 1;
		u32 res : 2;
		u32 discard_mode : 1;
		u32 pt_chunk_size : 10;
		u32 tr_cache_size : 10;
		u32 res2 : 8;
	};
	u32 full;
};

union HALF_OFFSET_type {
	struct {
		u32 fpu_pixel_half_offset : 1;
		u32 tsp_pixel_half_offset : 1;
		u32 texure_pixel_half_offset : 1;
	};
	u32 full;
};


/* Flycast calls the 21-bit parameter pointer in ISP_BACKGND_T `tag_address`;
 * libswirl calls it `param_offs_in_words`. Same bits, and refsw uses the
 * second name. */
#define param_offs_in_words tag_address

#undef ISP_FEED_CFG
#undef HALF_OFFSET
#define ISP_FEED_CFG   PvrReg(ISP_FEED_CFG_addr, ISP_FEED_CFG_type)
#define HALF_OFFSET    PvrReg(HALF_OFFSET_addr, HALF_OFFSET_type)
#define ISP_BACKGND_D  PvrReg(ISP_BACKGND_D_addr, ISP_BACKGND_D_type)

/* The fog colours: libswirl reads them as raw words and unpacks them itself,
 * Flycast overlays a colour type. refsw does the unpacking, so it gets the
 * words. */
#undef FOG_CLAMP_MIN
#undef FOG_CLAMP_MAX
#undef FOG_COL_RAM
#undef FOG_COL_VERT
#define FOG_CLAMP_MIN PvrReg(FOG_CLAMP_MIN_addr, u32)
#define FOG_CLAMP_MAX PvrReg(FOG_CLAMP_MAX_addr, u32)
#define FOG_COL_RAM   PvrReg(FOG_COL_RAM_addr, u32)
#define FOG_COL_VERT  PvrReg(FOG_COL_VERT_addr, u32)

/* Both projects carry a twiddle table under the same name (they are the same
 * table: the Dreamcast stores textures in Morton order and everyone who reads
 * them builds this). refsw keeps its own rather than borrowing Flycast's, so
 * it gets its own name. */
#define detwiddle refsw_detwiddle

/* libswirl's structured logging. A core has a frontend to talk to, not a
 * console, and a rasteriser that logs every tile is a rasteriser nobody can
 * profile - so these compile away entirely. */
#define V(x) (x)
#define JLOG(...) do { } while (0)
#define JLOG2(...) do { } while (0)
