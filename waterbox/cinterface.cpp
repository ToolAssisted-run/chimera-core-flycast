/* cinterface.cpp - Flycast behind the chimera guest ABI.
 *
 * Unlike every other core here, this one does not drive an upstream libretro
 * shell. Flycast's shell/libretro exists to serve RetroArch: GPU contexts,
 * option menus, threaded rendering, a UI. What a frontend actually needs is
 * underneath it, in core/emulator.h, and it is already the right shape:
 *
 *     init() -> loadGame(path) -> start() -> run() [one frame] -> term()
 *
 * So this file drives Emulator directly. The parts a sandbox cannot have -
 * a GPU, threads, sockets, a clock - are absent from the build rather than
 * disabled at runtime (see waterbox/sources.sh), and the few places upstream
 * insists on them are answered by patches/.
 *
 * M1 SCOPE: the machine runs and is identical native and sandboxed. It draws
 * NOTHING: Flycast has no software renderer, and porting one is its own
 * milestone (docs/PLAN.md). GetVideoBgra therefore hands back a blank frame of
 * the right size, and the gate compares memory and audio, not pixels.
 *
 * This file compiles IDENTICALLY for the guest (miniBox emulibc) and for the
 * native reference build (native-shim/emulibc.h), which is what makes the
 * equivalence gate a real proof.
 */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <emulibc.h>
#include <waterbox_settings.h>
#include <waterbox_slots.h>

#include "types.h"
#include "emulator.h"
#include "cfg/option.h"
#include "hw/maple/maple_devs.h"
#include "hw/maple/maple_cfg.h"
#include "hw/maple/maple_if.h"
#include "hw/mem/addrspace.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/aica/aica_if.h"
#include "hw/pvr/pvr_mem.h"

/* ---------------------------------------------------------------------------
 * What the frontend sees. A Dreamcast frame is 640x480 at 59.94Hz; M1 hands
 * back a blank one of that size so the frontend has something coherent to show
 * and the gate has a stable shape to hash.
 */
#define DC_WIDTH 640
#define DC_HEIGHT 480
#define MAX_SAMPLES 2048

static char g_loadError[512];
static uint32_t g_video[DC_WIDTH * DC_HEIGHT];
static int16_t g_soundOut[MAX_SAMPLES * 2];
static int g_nsamples;
static int g_inputRead;
static bool g_loaded;

/* the wire: one Dreamcast controller. Order is the frontend's button order and
 * must match waterbox.config. */
enum {
	BTN_A, BTN_B, BTN_X, BTN_Y, BTN_START,
	BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT,
	BTN_COUNT
};
static uint8_t g_setButtons[BTN_COUNT];
static uint8_t g_buttons[BTN_COUNT];

/* the analog wire: the stick and the two triggers, in the frontend's order */
enum { AXIS_X, AXIS_Y, AXIS_LTRIG, AXIS_RTRIG, AXIS_COUNT };
static int16_t g_axes[AXIS_COUNT];

/* Where a frontend's input actually enters the machine.
 *
 * There is a `kcode[4]` global in gamepad_device.h that looks like the answer
 * and is not: that one belongs to the desktop input layer, which reads
 * keyboards and joysticks and then FILLS mapleInputState. Writing to it links,
 * runs, and does nothing at all - the first sandboxed pad test read a
 * perfectly identical machine whether buttons were held or not. What the
 * controller reads is mapleInputState[player], so that is what a frontend
 * driving this core must write. */

/* ---------------------------------------------------------------------------
 * Lag detection: the machine looking at its input is what a lag frame IS.
 * patches/ adds a call to this where maple answers a controller read.
 */
extern "C" void chimera_input_was_read(void) { g_inputRead = 1; }

static void ApplyInput()
{
	MapleInputState& pad = mapleInputState[0];

	u32 code = ~0u; /* Dreamcast buttons are ACTIVE LOW */
	static const struct { int wire; u32 mask; } map[] = {
		{ BTN_A, DC_BTN_A }, { BTN_B, DC_BTN_B }, { BTN_X, DC_BTN_X },
		{ BTN_Y, DC_BTN_Y }, { BTN_START, DC_BTN_START },
		{ BTN_UP, DC_DPAD_UP }, { BTN_DOWN, DC_DPAD_DOWN },
		{ BTN_LEFT, DC_DPAD_LEFT }, { BTN_RIGHT, DC_DPAD_RIGHT },
	};
	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		if (g_buttons[map[i].wire]) code &= ~map[i].mask;
	pad.kcode = code;

	/* The triggers are half axes (0..255) and the sticks are full axes
	 * (-32768..32767); the frontend sends every axis as a signed 16-bit
	 * value, so the triggers are folded into their own range here. */
	pad.halfAxes[PJTI_L] = (u16)((g_axes[AXIS_LTRIG] + 32768) >> 8);
	pad.halfAxes[PJTI_R] = (u16)((g_axes[AXIS_RTRIG] + 32768) >> 8);
	pad.fullAxes[PJAI_X1] = g_axes[AXIS_X];
	pad.fullAxes[PJAI_Y1] = g_axes[AXIS_Y];
}

/* ---------------------------------------------------------------------------
 * the chimera guest ABI is a C ABI: the adapter looks these up by name
 */
extern "C" {

ECL_EXPORT const char *GetLoadError(void) { return g_loadError; }

ECL_EXPORT int Init(void)
{
	g_loadError[0] = '\0';

	/* the disc: the project slot's file, else the plain mount */
	char name[512];
	const char *file = "disc";
	if (wbx_slot_count("disc") > 0 && wbx_slot_name("disc", 0, name, sizeof(name)) != nullptr)
		file = name;

	try
	{
		emu.init();
		emu.loadGame(file);

		/* What a machine is must be a function of the project, so everything
		 * that would otherwise vary is pinned - and pinned HERE, after
		 * init() and loadGame(), because both of those load settings and
		 * would overwrite anything set before them.
		 *
		 * ThreadedRendering is the one that matters most: despite the name it
		 * decides whether start() launches an emulation THREAD, and a second
		 * thread stepping the same SH4 is not something a sandbox (or a
		 * movie) can survive. It cost an afternoon to find, because the
		 * symptom was an instruction trace that interleaved two executions of
		 * the same loop.
		 *
		 * The interpreter, not the dynarec: an equivalence gate compares
		 * emulation, and a recompiler is a separate question (docs/PLAN.md). */
		config::ThreadedRendering = false;
		config::DynarecEnabled = false;
		config::AutoLoadState = false;
		config::AutoSaveState = false;

		emu.start();
	}
	catch (const std::exception &e)
	{
		snprintf(g_loadError, sizeof(g_loadError), "%s", e.what());
		return 0;
	}
	catch (...)
	{
		snprintf(g_loadError, sizeof(g_loadError), "could not load '%s'", file);
		return 0;
	}

	g_loaded = true;
	return 1;
}

ECL_EXPORT void SetButton(int index, int value)
{
	if (index >= 0 && index < BTN_COUNT) g_setButtons[index] = value ? 1 : 0;
}

ECL_EXPORT void SetAxis(int index, int value)
{
	if (index >= 0 && index < AXIS_COUNT) g_axes[index] = (int16_t)value;
}

ECL_EXPORT void FrameAdvance(uint64_t packed)
{
	/* two input channels, and a frame is the UNION of them: a controller of
	 * 64 buttons or fewer arrives packed in this call, while the gate harness
	 * (and any wider controller) drives SetButton. */
	for (int i = 0; i < BTN_COUNT; i++)
		g_buttons[i] = g_setButtons[i] || ((packed >> i) & 1);

	g_inputRead = 0;
	g_nsamples = 0;
	ApplyInput();

	if (g_loaded)
		emu.run();
}

ECL_EXPORT uint32_t *GetVideoBgra(void) { return g_video; }
ECL_EXPORT int GetVideoWidth(void) { return DC_WIDTH; }
ECL_EXPORT int GetVideoHeight(void) { return DC_HEIGHT; }

ECL_EXPORT int16_t *GetAudio(void) { return g_soundOut; }
ECL_EXPORT int GetAudioSampleCount(void) { return g_nsamples; }

/* 59.94Hz, the NTSC Dreamcast's rate, as the exact ratio rather than a float */
ECL_EXPORT int GetVsyncNumerator(void) { return 60000; }
ECL_EXPORT int GetVsyncDenominator(void) { return 1001; }

ECL_EXPORT int InputWasRead(void) { return g_inputRead; }

/* ---------------------------------------------------------------------------
 * Memory domains. M1 exposes the one that matters for a movie: the 16MB of
 * system RAM. VRAM, the AICA's own RAM and the flash follow once the gate is
 * green on this one.
 */
/* The domains a movie and a watch window need. System RAM is the machine's
 * memory; VRAM is what the PVR draws from and where a renderer's output will
 * land (M3); the AICA's sound RAM is where samples and the sound CPU's own
 * program live. Each is a plain pointer into the guest's heap, because with no
 * virtual memory (see vmem-stub.cpp) that is exactly what Flycast allocated. */
struct Domain { const char *name; u8 *(*ptr)(); int64_t (*size)(); };

static u8 *RamPtr() { return &mem_b[0]; }
static int64_t RamSize() { return settings.platform.ram_size; }
static u8 *VramPtr() { return &vram[0]; }
static int64_t VramSize() { return settings.platform.vram_size; }
static u8 *AramPtr() { return &aica::aica_ram[0]; }
static int64_t AramSize() { return settings.platform.aram_size; }

static const Domain g_domains[] = {
	{ "System RAM", RamPtr, RamSize },
	{ "VRAM", VramPtr, VramSize },
	{ "Sound RAM", AramPtr, AramSize },
};
#define DOMAIN_COUNT ((int)(sizeof(g_domains) / sizeof(g_domains[0])))

ECL_EXPORT int GetMemoryDomainCount(void) { return DOMAIN_COUNT; }

ECL_EXPORT const char *GetMemoryDomainName(int which)
{
	return (which >= 0 && which < DOMAIN_COUNT) ? g_domains[which].name : nullptr;
}

ECL_EXPORT uint8_t *GetMemoryDomainPtr(int which)
{
	return (which >= 0 && which < DOMAIN_COUNT) ? g_domains[which].ptr() : nullptr;
}

ECL_EXPORT int64_t GetMemoryDomainSize(int which)
{
	return (which >= 0 && which < DOMAIN_COUNT) ? g_domains[which].size() : 0;
}

ECL_EXPORT int GetMemoryDomainWritable(int which)
{
	return (which >= 0 && which < DOMAIN_COUNT) ? 1 : 0;
}

/* M1 saves nothing: the VMU is a later milestone (docs/PLAN.md). */
ECL_EXPORT int32_t GetSaveDataFileCount(void) { return 0; }
ECL_EXPORT const char *GetSaveDataFileName(int32_t i) { (void)i; return nullptr; }
ECL_EXPORT int64_t GetSaveDataFileSize(int32_t i) { (void)i; return 0; }
ECL_EXPORT const uint8_t *GetSaveDataFileBuffer(int32_t i) { (void)i; return nullptr; }

} /* extern "C" */
