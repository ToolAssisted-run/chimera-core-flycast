/* What a sandbox answers differently from a desktop.
 *
 * Flycast is a whole application: a GUI, netplay, achievements, an audio
 * device, translations, a modem. A Chimera core is a machine and nothing else -
 * the frontend already owns everything above it - so those subsystems are not
 * compiled (waterbox/sources.sh says which and why), and what remains are the
 * symbols the machine still references.
 *
 * They are answered here rather than patched out of upstream, because a refusal
 * that lives in this repository is one that a pin bump cannot silently undo:
 * if upstream starts calling something new, the link fails and someone decides,
 * instead of a patch quietly not applying.
 *
 * Every function here is unreachable in a correctly configured core - nothing
 * enables netplay, nothing opens a settings window - so each one either does
 * nothing or reports "not available", and none of them lie about succeeding.
 */
#include "types.h"
#include "cfg/option.h"
#include "hw/maple/maple_devs.h"
#include "network/ggpo.h"
#include "network/net_handshake.h"
#include "hw/naomi/naomi_m3comm.h"
#include "hw/naomi/netdimm.h"
#include "oslib/oslib.h"
#include "audio/audiostream.h"
#include "oslib/resources.h"
#include "rend/TexCache.h"
#include "serialize.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

/* ---------------------------------------------------------------------------
 * The GUI. A frontend already exists; this core is the machine inside it.
 */
u32 gui_state = 0; /* GuiState::Closed */

void gui_init() {}
void gui_term() {}
void gui_open_settings() {}
void gui_open_onboarding() {}
void gui_cancel_load() {}
void gui_togglePause() {}
void gui_takeScreenshot() {}
void gui_saveState(bool, bool) {}
void gui_loadState(bool) {}
void gui_cycleSaveStateSlot(int) {}
bool gui_mouse_captured() { return false; }
void gui_set_mouse_position(int, int, bool) {}
void gui_set_mouse_button(int, bool, bool) {}
void gui_set_mouse_wheel(float) {}
void mainui_stop() {}
void push_vmu_screen(int, int, u8 *) {}

/* ---------------------------------------------------------------------------
 * Audio. The frontend takes samples from the ABI, not from a device: the
 * sandbox has neither a sound card nor a microphone.
 */
void InitAudio() {}
void TermAudio() {}
void WriteSample(s16, s16) {}
void StartAudioRecording(bool) {}
void StopAudioRecording() {}
u32 RecordAudio(void *, u32) { return 0; }

/* ---------------------------------------------------------------------------
 * Netplay, the modem, the broadband adapter, the NAOMI link. A movie is a
 * sequence of inputs to ONE machine; a machine that can be told things by a
 * network is not a machine a movie can replay.
 */
namespace ggpo {
	bool inRollback = false;
	void getInput(MapleInputState *) {}
	bool nextFrame() { return true; }
	bool active() { return false; }
	void endOfFrame() {}
}

bool naomiNetworkSupported() { return false; }
u16 defaultNaomiServerPort() { return 0; }
void NetworkHandshake::init() {}
void NetworkHandshake::term() {}
bool networkOutput = false;

u32 NaomiM3Comm::ReadMem(u32, u32) { return 0; }
void NaomiM3Comm::WriteMem(u32, u32, u32) {}
bool NaomiM3Comm::DmaStart(u32, u32) { return false; }
void NaomiM3Comm::closeNetwork() {}
/* The NET-DIMM is a NAOMI cartridge that boots its game over a network, from a
 * server that no longer exists. Its overrides are defined here so the class
 * still has a vtable; a project that names one gets a cartridge that refuses to
 * initialise rather than a link error in every other build. */
NetDimm::NetDimm(u32 size) : GDCartridge(size) {}
void NetDimm::Init(LoadProgress *progress, std::vector<u8> *digest)
{
	GDCartridge::Init(progress, digest);
}
bool NetDimm::Write(u32 offset, u32 size, u32 data)
{
	return GDCartridge::Write(offset, size, data);
}
void NetDimm::Deserialize(Deserializer& deser) { GDCartridge::Deserialize(deser); }
void NetDimm::process() {}
int NetDimm::schedCallback() { return 0; }

void ModemInit() {}
void ModemTerm() {}
void ModemReset() {}
u32 ModemReadMem_A0_006(u32, u32) { return 0; }
void ModemWriteMem_A0_006(u32, u32, u32) {}
void ModemSerialize(Serializer&) {}
void ModemDeserialize(Deserializer&) {}
void serialModemInit() {}
void serialModemTerm() {}

void bba_Init() {}
void bba_Term() {}
void bba_Reset(bool) {}
u32 bba_ReadMem(u32, u32) { return 0; }
void bba_WriteMem(u32, u32, u32) {}
void bba_Serialize(Serializer&) {}
void bba_Deserialize(Deserializer&) {}

/* ---------------------------------------------------------------------------
 * Retro achievements: an online service, and one that watches memory while a
 * game runs. Neither belongs in a machine a movie must replay identically.
 */
namespace achievements {
	void serialize(Serializer&) {}
	void deserialize(Deserializer&) {}
}

/* ---------------------------------------------------------------------------
 * Translations and resources: text for a UI this core does not draw, and fonts
 * read out of a zip it does not open.
 */
namespace i18n {
	const char *T(const char *s) { return s; }
	std::string Ts(const std::string& s) { return s; }
	void reloadLanguage() {}
}

namespace resource {
	std::unique_ptr<u8[]> load(const std::string&, size_t& size)
	{
		size = 0;
		return nullptr;
	}
}

/* ---------------------------------------------------------------------------
 * The host. A sandboxed guest cannot start processes, raise notifications or
 * break into a debugger; a fatal error is a fatal error, and saying so on
 * stderr is the most a core can do before the host takes over.
 */
void os_DebugBreak()
{
	fprintf(stderr, "[flycast] os_DebugBreak\n");
	abort();
}

void os_notify(const char *msg, int, const char *details)
{
	fprintf(stderr, "[flycast] %s%s%s\n", msg ? msg : "",
		details ? ": " : "", details ? details : "");
}

void os_RunInstance(int, const char **) {}

void fatal_error(const char *text, ...)
{
	va_list args;
	va_start(args, text);
	vfprintf(stderr, text, args);
	va_end(args);
	fputc('\n', stderr);
	abort();
}

/* ---------------------------------------------------------------------------
 * stb_image without thread-local storage. Custom textures are loaded flipped,
 * and stb offers a per-thread flag for that; with STBI_NO_THREAD_LOCALS (which
 * a sandbox needs - see meson.build) only the global one is compiled, and with
 * one thread they are the same flag.
 */
extern "C" void stbi_set_flip_vertically_on_load(int flag);
extern "C" void stbi_set_flip_vertically_on_load_thread(int flag)
{
	stbi_set_flip_vertically_on_load(flag);
}

/* A network gamepad. */
namespace dreampotato {
	void term() {}
	void update() {}
}
