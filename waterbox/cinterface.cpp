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
#include <vector>

#include <emulibc.h>
#include <waterbox_settings.h>
#include <waterbox_slots.h>

#include "types.h"
#include "emulator.h"
#include "cfg/option.h"
#include "cfg/cfg.h"
#include "hw/maple/maple_devs.h"
#if defined(CHIMERA_GUEST_GL)
#include "hw/pvr/Renderer_if.h"
/* At FILE scope on purpose: a declaration inside an ECL_EXPORT function body
 * inherits that function's C linkage, and these are defined as C++ in
 * waterbox/gl-osmesa.cpp. That mismatch has cost this project an afternoon
 * twice now. */
bool chimera_gl_start_osmesa();
bool chimera_gl_start_bridged();
void chimera_gl_bridge_offer(uint64_t (*bridge)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t));
bool chimera_gl_bridge_offered();

/* Whether an OpenGL renderer came up, either way. Asked by Renderer_if.cpp
 * when it picks a renderer and by the ABI when it goes looking for the
 * picture. Lives here rather than beside either context because only this file
 * knows which one was asked for. */
static bool g_glUp;
bool chimera_gl_available() { return g_glUp; }
#endif
#include "hw/maple/maple_cfg.h"
#include "hw/maple/maple_if.h"
#include "hw/mem/addrspace.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/aica/aica_if.h"
#include "hw/pvr/pvr_mem.h"
#include "hw/flashrom/nvmem.h"
#include "hw/pvr/Renderer_if.h"
#include "stdclass.h"

/* ---------------------------------------------------------------------------
 * What the frontend sees. A Dreamcast frame is 640x480 at 59.94Hz; M1 hands
 * back a blank one of that size so the frontend has something coherent to show
 * and the gate has a stable shape to hash.
 */
#define DC_WIDTH 640
#define DC_HEIGHT 480
#define MAX_SAMPLES 8192

static char g_loadError[512];
static uint32_t g_video[DC_WIDTH * DC_HEIGHT];
static int g_videoWidth = DC_WIDTH;
static int g_videoHeight = DC_HEIGHT;
static int16_t g_soundOut[MAX_SAMPLES * 2];
static int g_nsamples;
static int g_inputRead;
static bool g_loaded;

/* Turbo. Both renderers read this: the software one (waterbox/refsw-renderer.cpp)
 * leaves its tiles unrasterised, and Flycast's own OpenGL one (patches/0010)
 * skips the pass that would have reached a screen. A render-to-texture pass is
 * never skipped by either, because that one writes back into video memory and
 * the game reads it.
 *
 * extern "C" and not static because the patched upstream file names it.
 * ECL_INVISIBLE because it is the frontend's policy for the moment, not part of
 * the machine: a state saved while fast-forwarding must not put the machine
 * back into it when it is loaded to be looked at. */
extern "C" { ECL_INVISIBLE int chimera_render_enabled = 1; }

/* The wire: FOUR Dreamcast ports, each with the same superset of controls.
 * Order is the frontend's button order and must match waterbox.config - player
 * by player, and within a player exactly this list. */
#define DC_PORTS 4
enum {
	BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT,
	BTN_A, BTN_B, BTN_C, BTN_X, BTN_Y, BTN_Z, BTN_D, BTN_START,
	BTN_UP2, BTN_DOWN2, BTN_LEFT2, BTN_RIGHT2,
	BTN_RELOAD,
	BTN_MOUSE_LEFT, BTN_MOUSE_MIDDLE, BTN_MOUSE_RIGHT,
	BTN_PER_PORT
};
#define BTN_COUNT (BTN_PER_PORT * DC_PORTS)
static uint8_t g_setButtons[BTN_COUNT];
static uint8_t g_buttons[BTN_COUNT];

/* the analog wire, per port. Started at the NEUTRAL each axis declares in
 * waterbox.config, not at zero: a trigger's neutral is -32768 (released) and
 * its zero is half pressed. Chimera sends every axis every frame, so it never
 * sees the difference - but a core that reads "both triggers half held" until
 * somebody tells it otherwise is wrong on its own terms, and the gate drives
 * set_axis only when asked to. */
enum {
	AXIS_X, AXIS_Y, AXIS_LTRIG, AXIS_RTRIG,
	AXIS_X2, AXIS_Y2,
	AXIS_MOUSE_X, AXIS_MOUSE_Y, AXIS_MOUSE_WHEEL,
	AXIS_GUN_X, AXIS_GUN_Y,
	AXIS_PER_PORT
};
#define AXIS_COUNT (AXIS_PER_PORT * DC_PORTS)
static int16_t g_axes[AXIS_COUNT];

static void ResetAxesToNeutral()
{
	for (int p = 0; p < DC_PORTS; p++)
	{
		int16_t *a = &g_axes[p * AXIS_PER_PORT];
		for (int i = 0; i < AXIS_PER_PORT; i++) a[i] = 0;
		a[AXIS_LTRIG] = -32768;
		a[AXIS_RTRIG] = -32768;
	}
}

/* What each port is: MDT_None, or one of the devices waterbox.config offers.
 * Read from the settings once, at Init - a port's device is part of the
 * machine, not something that changes under a running movie. */
static MapleDeviceType g_portDevice[DC_PORTS];

/* The controller FAMILY - everything derived from maple_sega_controller, which
 * all read the same PlainJoystickState and differ only in what they report. A
 * mouse and a light gun are different wires and are not in it; nor do they have
 * expansion slots to put a memory card in, which is true of the real ones. */
static bool IsControllerFamily(MapleDeviceType t)
{
	return t == MDT_SegaController || t == MDT_AsciiStick
		|| t == MDT_TwinStick || t == MDT_SegaControllerXL;
}

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

/* Where the AICA's output leaves the machine. Flycast calls this once per
 * sample pair from the sound chip's mixer (sgc_if.cpp), and upstream's own
 * version stages 512 of them and hands the block to an AudioBackend - the shape
 * a sound card wants, and the wrong one here: it would deliver 512 samples on
 * one frame and 1024 on the next while the machine made 735, holding the
 * remainder back across the frame boundary. A frame-stepped core wants exactly
 * the samples of the frame it just ran.
 *
 * Note the argument order, which is upstream's: RIGHT first. The buffer is
 * interleaved left-right, as the ABI's audio channel is.
 *
 * g_nsamples is reset at the top of every FrameAdvance, so what is here is this
 * frame's sound and nothing else. A frame that somehow produced more than the
 * declared buffer holds keeps the first of them rather than running off the
 * end. The AICA runs at 44.1kHz, so a frame that took a sixtieth of a second
 * carries about 735 pairs - but a frame here ends when the machine PRESENTS,
 * and a game that renders slowly makes longer frames with proportionally more
 * sound in them. The buffer holds 8192, a fifth of a second. */
void WriteSample(s16 r, s16 l)
{
	if (g_nsamples >= MAX_SAMPLES)
		return;
	g_soundOut[g_nsamples * 2] = l;
	g_soundOut[g_nsamples * 2 + 1] = r;
	g_nsamples++;
}


static void ApplyInputPort(int port)
{
	MapleInputState& pad = mapleInputState[port];
	const uint8_t *btn = &g_buttons[port * BTN_PER_PORT];
	const int16_t *ax = &g_axes[port * AXIS_PER_PORT];
	const MapleDeviceType device = g_portDevice[port];

	u32 code = ~0u; /* Dreamcast buttons are ACTIVE LOW */
	static const struct { int wire; u32 mask; } map[] = {
		{ BTN_A, DC_BTN_A }, { BTN_B, DC_BTN_B }, { BTN_C, DC_BTN_C },
		{ BTN_X, DC_BTN_X }, { BTN_Y, DC_BTN_Y }, { BTN_Z, DC_BTN_Z },
		{ BTN_D, DC_BTN_D }, { BTN_START, DC_BTN_START },
		{ BTN_UP, DC_DPAD_UP }, { BTN_DOWN, DC_DPAD_DOWN },
		{ BTN_LEFT, DC_DPAD_LEFT }, { BTN_RIGHT, DC_DPAD_RIGHT },
		{ BTN_UP2, DC_DPAD2_UP }, { BTN_DOWN2, DC_DPAD2_DOWN },
		{ BTN_LEFT2, DC_DPAD2_LEFT }, { BTN_RIGHT2, DC_DPAD2_RIGHT },
		{ BTN_RELOAD, DC_BTN_RELOAD },
	};
	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		if (btn[map[i].wire]) code &= ~map[i].mask;
	pad.kcode = code;

	/* The mouse: three buttons of its own (ACTIVE LOW again) and RELATIVE
	 * motion, which is why it does not share the stick's axes. Flycast reads
	 * these as a delta accumulated since the last frame and clamps them itself
	 * (maple_mouse::mo_cvt, +-512), so what a frontend sends is the movement of
	 * this frame and not a position. */
	if (device == MDT_Mouse)
	{
		u8 buttons = ~0u;
		if (btn[BTN_MOUSE_LEFT]) buttons &= ~(1 << 2);
		if (btn[BTN_MOUSE_RIGHT]) buttons &= ~(1 << 1);
		if (btn[BTN_MOUSE_MIDDLE]) buttons &= ~(1 << 3);
		pad.mouseButtons = buttons;
		pad.relPos.x += ax[AXIS_MOUSE_X];
		pad.relPos.y += ax[AXIS_MOUSE_Y];
		pad.relPos.wheel += ax[AXIS_MOUSE_WHEEL];
	}

	/* The light gun: an absolute position on the screen, which the PVR turns
	 * into the scanline the gun was pointed at (spg.cpp). -32768..32767 is the
	 * frontend's range for every axis, so it is mapped onto the display here. */
	if (device == MDT_LightGun)
	{
		pad.absPos.x = (int)(((int32_t)ax[AXIS_GUN_X] + 32768) * 640 / 65536);
		pad.absPos.y = (int)(((int32_t)ax[AXIS_GUN_Y] + 32768) * 480 / 65536);
	}

	/* The sticks are full axes and the triggers are half axes: the frontend
	 * sends both as signed 16-bit, and a trigger's range is folded to unsigned
	 * here - released at -32768, fully pressed at 32767.
	 *
	 * UNSIGNED 16-BIT, not the 0..255 a controller reports. halfAxes is the
	 * whole 16-bit range and maple_cfg.cpp does the `>> 8` itself on the way to
	 * PlainJoystickState; handing it a value already reduced to 0..255 meant
	 * every trigger this machine has ever read was 0, fully pressed included.
	 * Nothing pointed at the triggers until Unreal Tournament, which fires with
	 * one (github #10) - the gate exercises buttons, and the fullAxes beside
	 * this line take the range the field actually wants, so the two lines
	 * looked like each other and were not. */
	pad.halfAxes[PJTI_L] = (u16)(ax[AXIS_LTRIG] + 32768);
	pad.halfAxes[PJTI_R] = (u16)(ax[AXIS_RTRIG] + 32768);
	pad.fullAxes[PJAI_X1] = ax[AXIS_X];
	pad.fullAxes[PJAI_Y1] = ax[AXIS_Y];
	/* The second stick, which only the PantherDC reports (FullController reads
	 * PJAI_X2/Y2 for its axes 4 and 5). Written for every device because the
	 * ones without it never look. */
	pad.fullAxes[PJAI_X2] = ax[AXIS_X2];
	pad.fullAxes[PJAI_Y2] = ax[AXIS_Y2];
}

/* Every port, every frame. A port set to 'none' has no device to read it, so
 * what is written there is never looked at - but it is written anyway, because
 * "the state of a port nobody asked about" is not something a movie should have
 * to think about. */
static void ApplyInput()
{
	for (int port = 0; port < DC_PORTS; port++)
		ApplyInputPort(port);
}

/* ---------------------------------------------------------------------------
 * THE MACHINE IS A FUNCTION OF THE PROJECT, not of the moment.
 *
 * A Dreamcast has a battery-backed clock, and Flycast reads the host's: the
 * time goes into RAM at boot, where the bios and games read it. Two runs of the
 * same movie would then start from two different machines - which the
 * equivalence gate saw immediately as twelve bytes of RAM differing between
 * native and sandbox while every other byte, and the program's own counter,
 * matched exactly.
 *
 * So the clock comes from the project. The default is the Dreamcast's own
 * epoch (1 January 1950), because a machine that always wakes at the same
 * moment is the point; a project that wants a particular date sets one.
 */
static uint32_t g_rtc;

extern "C" int chimera_pinned_rtc(uint32_t *rtc)
{
	if (rtc) *rtc = g_rtc;
	return 1;
}

/* The console's own identity, which some games read (and one of them, ChuChu
 * Rocket, uses on a network). Upstream generates it with the C library's rand()
 * seeded from the clock - and glibc and musl do not agree on rand(), so the
 * sandboxed machine came up with a different console than the native one. It
 * is derived from the pinned clock instead, by arithmetic this core does
 * itself, so both flavors reach the same six bytes. */
extern "C" int chimera_pinned_console_id(uint8_t id[6])
{
	uint32_t x = g_rtc ^ 0x5DEECE66u;
	for (int i = 0; i < 6; i++)
	{
		x = x * 1103515245u + 12345u;
		id[i] = (uint8_t)(x >> 16);
	}
	return 1;
}

/* A setting whose value is one of a list, as an index into that list. The
 * package declares the names (see waterbox.config); Flycast wants the numbers.
 */
static int SettingIndex(const char *name, const char *const *options, int count, int fallback)
{
	char value[32];
	strncpy(value, options[fallback], sizeof(value) - 1);
	value[sizeof(value) - 1] = '\0';
	wbx_setting_str(name, value, sizeof(value));
	for (int i = 0; i < count; i++)
		if (!strcmp(value, options[i]))
			return i;
	return fallback;
}

/* What kind of Dreamcast this is. All three live in the machine's flash, all
 * three are things a game can read and act on, and none of them is a display
 * preference - so they are part of the project.
 *
 * This is applied TWICE, and has to be. The flash is built during init(), so
 * the region and language must be set before then or the machine is stamped
 * with defaults; and init() and loadGame() both load settings of their own,
 * which would overwrite anything set before them. Setting them on both sides
 * is the honest way to be sure, given Flycast expects a front end that owns a
 * config file and this core has none.
 */
static void ApplyMachineSettings()
{
	static const char *const regions[] = { "japan", "usa", "europe" };
	static const char *const languages[] = { "japanese", "english", "german",
		"french", "spanish", "italian" };
	static const char *const broadcasts[] = { "ntsc", "pal", "palM", "palN" };

	/* Through Flycast's own config, not by assigning to the options.
	 * loadGame() RESETS every option and reloads them from the config
	 * immediately before it builds the machine's flash, so an assignment made
	 * beforehand is thrown away and one made afterwards is too late for the
	 * flash - which is where the region and language actually live. A
	 * TRANSIENT config entry is the mechanism for a front end that has no
	 * config file: the reload reads it, and nothing ever writes it out. */
	/* section "config", key "Dreamcast.Region": an Option's section defaults to
	 * "config" and its name is the whole dotted string, which is not what the
	 * dot suggests. */
	config::setTransient("config", "Dreamcast.Region",
		std::to_string(SettingIndex("region", regions, 3, 1)));
	config::setTransient("config", "Dreamcast.Language",
		std::to_string(SettingIndex("language", languages, 6, 1)));
	config::setTransient("config", "Dreamcast.Broadcast",
		std::to_string(SettingIndex("broadcast", broadcasts, 4, 0)));

	/* What is plugged into each of the four ports.
	 *
	 * TRANSIENT, and for a harder reason than the region's. The maple devices
	 * are BUILT inside loadGame (emulator.cpp calls mcfg_CreateDevices there),
	 * so a port assigned afterwards is a value nothing will ever read again -
	 * the machine already has whatever the defaults said. Which is exactly what
	 * happened while this was written the obvious way: four ports were set to
	 * 'gamepad', the 240p Suite was asked what was connected, and it answered
	 * one controller and three empty sockets.
	 *
	 * The defaults are worth knowing too: device1 is a controller and BOTH of
	 * its expansion slots are memory cards, which is why a one-player machine
	 * has always exported vmu_A1 and vmu_A2. A card goes in the first slot of
	 * anything that has one - the controller family - and the second slot is
	 * left empty, because two cards per player is the emulator's habit and not
	 * the machine's. A mouse and a light gun have no slots at all on the real
	 * thing (maple_getPortCount), and get none here. */
	static const char *const devices[] = {
		"none", "gamepad", "arcadeStick", "twinStick", "xl", "mouse", "lightGun"
	};
	static const MapleDeviceType types[] = {
		MDT_None, MDT_SegaController, MDT_AsciiStick, MDT_TwinStick,
		MDT_SegaControllerXL, MDT_Mouse, MDT_LightGun
	};
	for (int port = 0; port < DC_PORTS; port++)
	{
		char key[16];
		snprintf(key, sizeof(key), "port%d", port + 1);
		g_portDevice[port] = types[SettingIndex(key, devices, 7, port == 0 ? 1 : 0)];

		const MapleDeviceType slot0 =
			IsControllerFamily(g_portDevice[port]) ? MDT_SegaVMU : MDT_None;

		snprintf(key, sizeof(key), "device%d", port + 1);
		config::setTransient("input", key, std::to_string((int)g_portDevice[port]));
		snprintf(key, sizeof(key), "device%d.1", port + 1);
		config::setTransient("input", key, std::to_string((int)slot0));
		snprintf(key, sizeof(key), "device%d.2", port + 1);
		config::setTransient("input", key, std::to_string((int)MDT_None));
	}
}

/* Which bios this machine runs, which is a PROJECT decision rather than a
 * question about what files happen to be lying around: a machine with its own
 * bios and one with an HLE reimplementation are different machines and do not
 * share movies. The setting says which; the frontend guarantees the files are
 * mounted when it says "real" (see waterbox.config's firmware conditions). */
static bool UseRealBios()
{
	static const char *const options[] = { "hle", "real" };
	return SettingIndex("bios", options, 2, 0) == 1;
}

/* ---------------------------------------------------------------------------
 * the chimera guest ABI is a C ABI: the adapter looks these up by name
 */
extern "C" {

ECL_EXPORT const char *GetLoadError(void) { return g_loadError; }

ECL_EXPORT int Init(void)
{
	g_loadError[0] = '\0';
	ResetAxesToNeutral();

	/* Save data the project brought is mounted under its own name and the
	 * machine picks it up as it builds each card (chimera_register_vmu). What
	 * happens here is a REFUSAL: a card named something the machine never opens
	 * would be carried by the project, pinned by the movie, and silently
	 * ignored, which is worse than not booting. */
	{
		char entry[512];
		const int32_t saves = wbx_slot_count("savedata");
		for (int32_t i = 0; i < saves; i++)
		{
			if (wbx_slot_name("savedata", i, entry, sizeof(entry)) == nullptr)
				continue;
			if (strncmp(entry, "vmu_", 4) != 0)
			{
				snprintf(g_loadError, sizeof(g_loadError),
					"this machine does not read save data called \"%s\". Its cards are "
					"vmu_A1.bin through vmu_D1.bin, one per connected controller - the "
					"names Export Save Data writes.", entry);
				return 0;
			}
		}
	}

	/* the disc: the project slot's file, else the plain mount */
	char name[512];
	const char *file = "disc";
	if (wbx_slot_count("disc") > 0 && wbx_slot_name("disc", 0, name, sizeof(name)) != nullptr)
		file = name;

	/* Where Flycast looks for the things a machine is made of. The sandbox
	 * mounts whatever the project supplies at the root of the guest's file
	 * system, so that is the only place to look - and the only place that
	 * exists. A real bios (dc_boot.bin, dc_flash.bin) arrives through
	 * Chimera's firmware channel; when the project has none, the HLE bios in
	 * core/reios runs instead, which is what every gate here uses. */
	g_rtc = (uint32_t)wbx_setting_long("rtc", 0);

	set_user_data_dir("./");
	add_system_data_dir("./");
	config::UseReios = !UseRealBios();

	ApplyMachineSettings();

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

		ApplyMachineSettings();

#if defined(CHIMERA_GUEST_GL)
		/* Which renderer draws. Both are software - one is skmp's reference
		 * rasteriser, the other is Flycast's own OpenGL renderer running
		 * against a Mesa softpipe compiled into this guest
		 * (waterbox/gl-osmesa.cpp). The OpenGL one is what upstream tests and
		 * what gets a game's picture right. Neither leaves the sandbox and
		 * neither asks the machine it runs on anything, so a movie made under
		 * one replays under the other.
		 *
		 * It goes HERE, after init() and loadGame(), and not before them:
		 * bringing Mesa up first moves every allocation Flycast then makes,
		 * and addrspace::initMappings wrote off the end of the guest. */
		{
			static const char *const renderers[] = { "software", "opengl", "opengl-hw" };
			const int choice = SettingIndex("renderer", renderers, 3, 0);
			if (choice == 2 && chimera_gl_bridge_offered())
			{
				/* "opengl-hw": the same renderer, drawing on a GPU outside the
				 * sandbox. Not deterministic, and asked for explicitly. */
				g_glUp = chimera_gl_start_bridged();
				if (!g_glUp)
					fprintf(stderr, "chimera: the GPU bridge would not start, falling back\n");
			}
			if (!g_glUp && choice >= 1)
			{
				/* Either "opengl", or "opengl-hw" on a Chimera that offered no
				 * bridge - which is any build without one, and any machine
				 * whose driver would not give a context. The softpipe draws,
				 * the picture is right, and the run stays deterministic. */
				g_glUp = chimera_gl_start_osmesa();
				if (!g_glUp)
					fprintf(stderr, "chimera: OpenGL would not start, drawing in software\n");
			}
		}
#endif

		/* Bring the renderer up. On a desktop this is the graphics context's
		 * job - whoever owns the window creates the device and then calls
		 * this - and with no window nobody does, which leaves the renderer
		 * pointer null and the first render a segfault inside Flycast. The
		 * software renderer needs no device, so it is simply started here. */
		if (!rend_init_renderer())
		{
			snprintf(g_loadError, sizeof(g_loadError), "the software renderer would not start");
			return 0;
		}

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
	/* Two input channels, and a frame is the UNION of them: the first 64
	 * buttons arrive packed in this call, while the gate harness (and any
	 * controller wider than 64) drives SetButton.
	 *
	 * FOUR PORTS IS WIDER THAN 64. Shifting a uint64_t by 64 or more is
	 * undefined, not zero, so the packed channel stops where it runs out and
	 * the rest of the wire is SetButton's alone - which is what Chimera uses
	 * for a controller this wide anyway. */
	for (int i = 0; i < BTN_COUNT; i++)
		g_buttons[i] = g_setButtons[i] || (i < 64 && ((packed >> i) & 1));

	g_inputRead = 0;
	g_nsamples = 0;
	ApplyInput();

	if (g_loaded)
		emu.run();
}

/* The picture, from the software renderer (waterbox/refsw-renderer.cpp). A
 * frame the machine never rendered leaves the last one standing, which is what
 * the hardware does too: the video hardware keeps scanning out whatever is in
 * the framebuffer. */
extern "C" const uint32_t *chimera_refsw_frame(int *width, int *height);

ECL_EXPORT uint32_t *GetVideoBgra(void)
{
#if defined(CHIMERA_GUEST_GL)
	if (chimera_gl_available())
	{
		/* Flycast already knows how to get its own picture back: the renderer
		 * keeps it in a framebuffer of its own and hands it over through
		 * GetLastFrame, which is what a screenshot takes. Reading framebuffer
		 * 0 instead reads whatever was left lying there. It arrives as RGB
		 * rows; the frontend wants BGRA. */
		static std::vector<u8> rgb;
		int w = 0, h = 0;
		if (renderer != nullptr && renderer->GetLastFrame(rgb, w, h) && w > 0 && h > 0)
		{
			if (w > DC_WIDTH) w = DC_WIDTH;
			if (h > DC_HEIGHT) h = DC_HEIGHT;
			g_videoWidth = w;
			g_videoHeight = h;

			for (int y = 0; y < h; y++)
			{
				const uint8_t *src = &rgb[(size_t)y * w * 3];
				uint32_t *dst = &g_video[(size_t)y * w];
				for (int x = 0; x < w; x++, src += 3)
					dst[x] = 0xFF000000u | (src[0] << 16) | (src[1] << 8) | src[2];
			}
		}
		return g_video;
	}
#endif

	int w = 0, h = 0;
	const uint32_t *frame = chimera_refsw_frame(&w, &h);
	g_videoWidth = w;
	g_videoHeight = h;
	memcpy(g_video, frame, (size_t)w * h * sizeof(uint32_t));
	return g_video;
}

/* Turbo (optional guest ABI group): while off the core must produce no picture
 * and must otherwise be exactly the machine it would have been. run-gate.sh's
 * turbo leg is the proof - N undrawn frames plus one drawn one come out byte for
 * byte the same machine, and the same picture, as N+1 drawn ones. */
ECL_EXPORT void SetRenderingEnabled(int on) { chimera_render_enabled = on != 0; }

ECL_EXPORT int GetVideoWidth(void) { return g_videoWidth; }
ECL_EXPORT int GetVideoHeight(void) { return g_videoHeight; }

/* The AICA's samples reach the frame buffer through WriteSample, which is
 * defined above the export block (C++ linkage - Flycast declares it). */
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

static u8 *FlashPtr() { return nvmem::getFlashData(); }
static int64_t FlashSize() { return settings.platform.flash_size; }

static const Domain g_domains[] = {
	{ "System RAM", RamPtr, RamSize },
	{ "VRAM", VramPtr, VramSize },
	{ "Sound RAM", AramPtr, AramSize },
	/* The flash is where the machine keeps what it is: region, language, the
	 * date, the console id, and whatever a game wrote to the system area. A
	 * watch window that cannot see it cannot see why two machines differ. */
	{ "Flash", FlashPtr, FlashSize },
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

/* ---------------------------------------------------------------------------
 * Save data: the memory cards.
 *
 * A Dreamcast keeps saves on a VMU, which is a 128KB flash chip inside the
 * controller - and on a desktop Flycast keeps that in a file next to the
 * emulator. A sandboxed core has nowhere to put a file, and a movie that
 * depends on a save nobody can see is not reproducible, so patches/0002 hands
 * the flash over here instead and it travels through Chimera's save-data
 * channel like any other persistent data.
 */
#define MAX_VMUS 4

static struct {
	char name[32];
	uint8_t *flash;
	unsigned size;
} g_vmus[MAX_VMUS];
static int g_vmuCount;

} /* extern "C" */

extern "C" bool chimera_register_vmu(const char *port, uint8_t *flash, unsigned size)
{
	if (g_vmuCount >= MAX_VMUS)
		return false;
	snprintf(g_vmus[g_vmuCount].name, sizeof(g_vmus[g_vmuCount].name), "vmu_%s.bin", port);
	g_vmus[g_vmuCount].flash = flash;
	g_vmus[g_vmuCount].size = size;
	g_vmuCount++;

	/* What the project starts with already saved on this card.
	 *
	 * A card the frontend mounted under the name this one exports is read
	 * straight into the flash the machine is about to use. It happens HERE
	 * because here is where the machine is being built - during Init, before
	 * seal - so the contents land in the sealed baseline and a savestate
	 * carries only what the game has written since, not the card.
	 *
	 * Upstream would have opened its own file for this; a sandboxed core has
	 * no save directory, and the project is where saves come from. Nothing
	 * mounted means a fresh card, which Flycast formats for itself. */
	if (FILE *f = fopen(g_vmus[g_vmuCount - 1].name, "rb"))
	{
		const size_t got = fread(flash, 1, size, f);
		fclose(f);
		if (got != size)
			fprintf(stderr, "chimera: %s is %zu bytes, not %u; the rest is left blank\n",
				g_vmus[g_vmuCount - 1].name, got, size);
	}
	return true;
}

extern "C" {

ECL_EXPORT int32_t GetSaveDataFileCount(void) { return g_vmuCount; }

ECL_EXPORT const char *GetSaveDataFileName(int32_t i)
{
	return (i >= 0 && i < g_vmuCount) ? g_vmus[i].name : nullptr;
}

ECL_EXPORT int64_t GetSaveDataFileSize(int32_t i)
{
	return (i >= 0 && i < g_vmuCount) ? g_vmus[i].size : 0;
}

ECL_EXPORT const uint8_t *GetSaveDataFileBuffer(int32_t i)
{
	return (i >= 0 && i < g_vmuCount) ? g_vmus[i].flash : nullptr;
}

#if defined(CHIMERA_GUEST_GL)
/* A host with a real GL context offers it here, BEFORE Init, because Init is
 * where the renderer is chosen. Taking the offer is a separate decision made
 * there, from the project's renderer setting: a core handed a GPU that nobody
 * asked for still draws in software. */
ECL_EXPORT void SetGpuBridge(uint64_t addr)
{
	chimera_gl_bridge_offer((uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t))addr);
}
#endif

} /* extern "C" */
