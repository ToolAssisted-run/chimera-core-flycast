/* Every texture format decodes to the SAME colour order.
 *
 * The Dreamcast stores a texel in one of several formats, and the software
 * renderer has a decoder for each. They must all agree about which byte of the
 * 32-bit result is blue, because the frontend is handed those bytes directly
 * (GetVideoBgra: byte 0 blue, byte 1 green, byte 2 red, byte 3 alpha). One of
 * them did not: libswirl's packRGB, which only the YUV path uses, put red in
 * the low byte, so a cutscene - the one thing a Dreamcast draws from YUV -
 * came out with its reds and blues exchanged (chimera#90).
 *
 * So this decodes ONE known colour through every format and checks where the
 * channels land. It needs no machine and no disc; run-gate.sh runs it first.
 */
#include "types.h"
#include "refsw/TexUtils.h"
#include "hw/pvr/ta_structs.h"
#include "hw/pvr/pvr_regs.h"

extern void chimera_refsw_decode(u32 tsp, u32 tcw, u32 *out, int w, int h);

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static int failed;

static void check(bool ok, const char *what, u32 got)
{
	printf("%s %-46s %08x\n", ok ? "PASS" : "FAIL", what, got);
	if (!ok) failed++;
}

/* what the frontend reads out of a 32-bit texel */
static u8 blue(u32 c) { return (u8)(c & 0xFF); }
static u8 green(u32 c) { return (u8)((c >> 8) & 0xFF); }
static u8 red(u32 c) { return (u8)((c >> 16) & 0xFF); }
static u8 alpha(u32 c) { return (u8)((c >> 24) & 0xFF); }

/* A colour is "mostly red" if red dominates and blue does not, whatever the
 * format's precision: the point is which CHANNEL, never the exact value. */
static bool mostlyRed(u32 c) { return red(c) > 200 && blue(c) < 60; }
static bool mostlyBlue(u32 c) { return blue(c) > 200 && red(c) < 60; }
static bool mostlyYellow(u32 c) { return red(c) > 180 && green(c) > 180 && blue(c) < 80; }


/* one known colour, in each 16-bit format. They are file scope because the
 * paletted walk below loads them into a palette and decodes them again. */
static const u16 red1555 = 0x8000 | (31 << 10), blue1555 = 0x8000 | 31;
static const u16 red565 = 31 << 11, blue565 = 31;
static const u16 red4444 = 0xF000 | (15 << 8), blue4444 = 0xF000 | 15;

/* ---------------------------------------------------------------------------
 * A PALETTED texel is expanded ONCE.
 *
 * refsw hands the raw palette word back from DecodeTextel and TextureFetch
 * expands it with GetExpandFormat, which for a paletted texture is the format
 * PAL_RAM_CTRL names. Unpacking the palette a second time on the way in was
 * tried and Street Fighter Zero 3's sprites came out magenta, so the whole
 * path is walked here: a palette entry, a texture of one index, and the colour
 * that reaches the screen. Needs no machine and no disc - only the register
 * file, eight megabytes of pretend VRAM and one index.
 */
/* The world the rasteriser's texture path reads, and nothing else: the PVR's
 * register file, the VRAM behind it, and the two host calls it can make on a
 * path it will not take here. Providing them is what keeps this test free of
 * a machine. */
u8 pvr_regs[pvr_RegSize];
u8 *emu_vram;
settings_t settings;
void os_DebugBreak() { abort(); }
void fatal_error(const char *text, ...) { (void)text; abort(); }

static u32 decodeOnePal8(u16 entry, u32 palFormat)
{
	const u8 index = 0x2A;
	memset(emu_vram, index, 1024);
	PAL_RAM_CTRL = palFormat;
	u32 *pal = &PvrReg(PALETTE_RAM_START_addr, u32);
	memset(pal, 0, 1024 * sizeof(u32));
	pal[index] = entry;

	TCW tcw {}; tcw.full = 0;
	tcw.PixelFmt = PixelPal8;	/* TexAddr 0, PalSelect 0, not twiddled-VQ */
	TSP tsp {}; tsp.full = 0;	/* an 8x8 texture, point sampled */

	u32 out[4 * 4] {};
	chimera_refsw_decode(tsp.full, tcw.full, out, 4, 4);
	return out[0];
}

static void paletted()
{
	static std::vector<u8> vram(8 * 1024 * 1024);
	emu_vram = vram.data();
	settings.platform.vram_mask = (u32)vram.size() - 1;

	/* the same red, in each format a palette may be loaded in */
	const u32 p15 = decodeOnePal8(red1555, 0);
	const u32 p56 = decodeOnePal8(red565, 1);
	const u32 p44 = decodeOnePal8(red4444, 2);
	check(mostlyRed(p15), "paletted ARGB1555 entry is red", p15);
	check(mostlyRed(p56), "paletted RGB565 entry is red", p56);
	check(mostlyRed(p44), "paletted ARGB4444 entry is red", p44);

	/* and it is EXACTLY the entry expanded once - expanded twice, the first
	 * expansion's high bytes become the second's channels and red turns to
	 * something else entirely */
	check(p15 == ARGB1555_32(red1555), "paletted ARGB1555 is expanded exactly once", p15);
	check(p56 == ARGB565_32(red565), "paletted RGB565 is expanded exactly once", p56);
	check(p44 == ARGB4444_32(red4444), "paletted ARGB4444 is expanded exactly once", p44);

	/* blue too, so a channel swap cannot pass by symmetry */
	const u32 b15 = decodeOnePal8(blue1555, 0);
	check(mostlyBlue(b15), "paletted ARGB1555 entry is blue", b15);
}

int main()
{
	/* The words go into variables first: these are MACROS, and their argument
	 * is not parenthesised, so handing one `0xF000 | (15 << 8)` lets the
	 * shifts inside bind to the wrong half of it. (Found by this test reading
	 * an alpha of zero for an opaque texel, which is worth keeping in mind
	 * for anyone adding a case.) */

	/* red, opaque, in each of the direct formats */
	const u32 r15 = ARGB1555_32(red1555), r56 = ARGB565_32(red565), r44 = ARGB4444_32(red4444);
	check(mostlyRed(r15), "ARGB1555 red is red", r15);
	check(mostlyRed(r56), "RGB565 red is red", r56);
	check(mostlyRed(r44), "ARGB4444 red is red", r44);
	/* blue, so a swap cannot pass by symmetry */
	const u32 b15 = ARGB1555_32(blue1555), b56 = ARGB565_32(blue565), b44 = ARGB4444_32(blue4444);
	check(mostlyBlue(b15), "ARGB1555 blue is blue", b15);
	check(mostlyBlue(b56), "RGB565 blue is blue", b56);
	check(mostlyBlue(b44), "ARGB4444 blue is blue", b44);
	/* and an opaque texel reads opaque in the format that carries alpha */
	check(alpha(r44) == 0xF0, "ARGB4444 alpha is opaque", r44);

	/* YUV: the colour a video decoder writes for yellow (BT.601-ish, which is
	 * what refsw's own coefficients invert). Yellow is the case that showed
	 * the bug, because red and blue exchanged turns it cyan-blue. */
	const u32 yellow = YUV422(226, 16, 149);
	check(mostlyYellow(yellow), "YUV422 yellow is yellow, not blue", yellow);
	const u32 yuvRed = YUV422(82, 90, 240);
	check(mostlyRed(yuvRed), "YUV422 red is red", yuvRed);
	const u32 yuvBlue = YUV422(41, 240, 110);
	check(mostlyBlue(yuvBlue), "YUV422 blue is blue", yuvBlue);

	/* alpha: opaque must read opaque in every format */
	const u16 opaque1555 = 0x8000; const u16 zero565 = 0;
	const u32 a15 = ARGB1555_32(opaque1555), a56 = ARGB565_32(zero565);
	check(alpha(a15) == 0xFF, "ARGB1555 alpha bit is opaque", a15);
	check(alpha(a56) == 0xFF, "RGB565 has no alpha and is opaque", a56);
	check(alpha(yellow) == 0xFF, "YUV422 is opaque", yellow);

	paletted();

	printf("%s: %d failed\n", failed ? "FAIL" : "PASS", failed);
	return failed ? 1 : 0;
}
