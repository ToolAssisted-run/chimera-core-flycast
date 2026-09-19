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

#include <cstdio>
#include <cstdlib>

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

int main()
{
	/* The words go into variables first: these are MACROS, and their argument
	 * is not parenthesised, so handing one `0xF000 | (15 << 8)` lets the
	 * shifts inside bind to the wrong half of it. (Found by this test reading
	 * an alpha of zero for an opaque texel, which is worth keeping in mind
	 * for anyone adding a case.) */
	const u16 red1555 = 0x8000 | (31 << 10), blue1555 = 0x8000 | 31;
	const u16 red565 = 31 << 11, blue565 = 31;
	const u16 red4444 = 0xF000 | (15 << 8), blue4444 = 0xF000 | 15;

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

	printf("%s: %d failed\n", failed ? "FAIL" : "PASS", failed);
	return failed ? 1 : 0;
}
