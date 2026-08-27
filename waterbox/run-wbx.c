/* run-wbx.c - drives core.wbx through the miniBox host over the same work
 * dir and schedule as run-native, reporting the same digests, so the two
 * builds diff directly. Every regular file in the work dir is mounted into
 * the guest under its basename - exactly what the frontend does with a
 * project's files, slot map and settings.
 *
 * usage: run-wbx <core.wbx> <workdir> [run-native's options] [--rerecord]
 *
 * --rerecord round-trips the WHOLE guest machine through the host's
 * save/load state around every frame; the digests must be identical.
 */
#include "minibox.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "gate-harness.h"

typedef struct { FILE *f; } freader;
static intptr_t file_read(uintptr_t ud, uint8_t *d, uintptr_t s) { return (intptr_t)fread(d, 1, s, ((freader *)ud)->f); }
typedef struct { uint8_t *b; size_t len, cap, pos; } membuf;
static int32_t mem_write(uintptr_t ud, const uint8_t *d, uintptr_t n)
{
	membuf *m = (membuf *)ud;
	if (m->len + n > m->cap) { m->cap = (m->len + n) * 2 + 64; m->b = realloc(m->b, m->cap); }
	memcpy(m->b + m->len, d, n); m->len += n; return 0;
}
static intptr_t mem_read(uintptr_t ud, uint8_t *d, uintptr_t n)
{
	membuf *m = (membuf *)ud;
	uintptr_t avail = m->len - m->pos; if (n > avail) n = avail;
	memcpy(d, m->b + m->pos, n); m->pos += n; return (intptr_t)n;
}

typedef int (MB_GUEST_ABI *intfn)(void);
typedef void (MB_GUEST_ABI *framefn)(uint64_t);
typedef void (MB_GUEST_ABI *setfn)(int32_t, int32_t);
typedef uintptr_t (MB_GUEST_ABI *ptrfn)(void);
typedef uintptr_t (MB_GUEST_ABI *ptrfn_i)(int);
typedef int (MB_GUEST_ABI *intfn_i)(int);
typedef int64_t (MB_GUEST_ABI *i64fn_i)(int);
typedef int32_t (MB_GUEST_ABI *i32fn)(void);
typedef int32_t (MB_GUEST_ABI *i32fn_i)(int32_t);
typedef uintptr_t (MB_GUEST_ABI *ptrfn_i32)(int32_t);
typedef int64_t (MB_GUEST_ABI *i64fn_i32)(int32_t);

static mb_host *g_host;
static intfn g_Init;
static ptrfn g_GetLoadError;
static setfn g_SetButton;
static setfn g_SetAxis;
static framefn g_FrameAdvance;
static ptrfn g_GetVideoBgra;
static intfn g_GetVideoWidth, g_GetVideoHeight;
static ptrfn g_GetAudio;
static intfn g_GetAudioSampleCount;
static intfn g_InputWasRead;
static intfn g_GetMemoryDomainCount;
static ptrfn_i g_GetMemoryDomainName, g_GetMemoryDomainPtr;
static i64fn_i g_GetMemoryDomainSize;
static intfn g_GetVsyncNumerator, g_GetVsyncDenominator;
static i32fn g_GetSaveDataFileCount;
static ptrfn_i32 g_GetSaveDataFileName, g_GetSaveDataFileBuffer;
static i64fn_i32 g_GetSaveDataFileSize;
static int g_rerecord;
static membuf g_state;

static uintptr_t proc(mb_host *h, const char *n)
{
	mb_return r;
	wbx_get_proc_addr(h, n, &r);
	if (r.error_message[0]) { fprintf(stderr, "proc %s: %s\n", n, r.error_message); exit(2); }
	if (!r.data) { fprintf(stderr, "missing required export %s\n", n); exit(2); }
	return r.data;
}

static int core_init(void) { return g_Init(); }
static int core_init_done(void) { return 1; } /* boot happens before Seal, not in gate_run */
static const char *core_load_error(void) { return (const char *)g_GetLoadError(); }
static void core_set_button(int32_t i, int32_t s) { g_SetButton(i, s); }
static void core_set_axis(int32_t i, int32_t v) { g_SetAxis(i, v); }
static void core_frame(void) { g_FrameAdvance(0); }
static const uint32_t *core_video(int *w, int *h)
{
	*w = g_GetVideoWidth();
	*h = g_GetVideoHeight();
	return (const uint32_t *)g_GetVideoBgra();
}
static const int16_t *core_audio(int *n)
{
	*n = g_GetAudioSampleCount();
	return (const int16_t *)g_GetAudio();
}
static int core_input_was_read(void) { return g_InputWasRead(); }
static int core_domain_count(void) { return g_GetMemoryDomainCount(); }
static int core_vsync_numerator(void) { return g_GetVsyncNumerator(); }
static int core_vsync_denominator(void) { return g_GetVsyncDenominator(); }
static int32_t core_savedata_count(void) { return g_GetSaveDataFileCount(); }
static const char *core_savedata_name(int32_t i) { return (const char *)g_GetSaveDataFileName(i); }
static int64_t core_savedata_size(int32_t i) { return g_GetSaveDataFileSize(i); }
static const uint8_t *core_savedata_buffer(int32_t i) { return (const uint8_t *)g_GetSaveDataFileBuffer(i); }
static const char *core_domain_name(int i) { return (const char *)g_GetMemoryDomainName(i); }
static const uint8_t *core_domain_ptr(int i) { return (const uint8_t *)g_GetMemoryDomainPtr(i); }
static int64_t core_domain_size(int i) { return g_GetMemoryDomainSize(i); }

static void core_pre_frame(void)
{
	if (!g_rerecord)
		return;
	mb_return r;
	g_state.len = 0;
	wbx_save_state(g_host, mem_write, (uintptr_t)&g_state, &r);
	if (r.error_message[0]) { fprintf(stderr, "save_state: %s\n", r.error_message); exit(1); }
	g_state.pos = 0;
	wbx_load_state(g_host, mem_read, (uintptr_t)&g_state, &r);
	if (r.error_message[0]) { fprintf(stderr, "load_state: %s\n", r.error_message); exit(1); }
}

int main(int argc, char **argv)
{
	if (argc < 3)
	{
		fprintf(stderr, "usage: run-wbx <core.wbx> <workdir> [options] [--rerecord]\n");
		return 2;
	}
	const char *wbxPath = argv[1];
	const char *workdir = argv[2];
	for (int i = 3; i < argc; i++)
		if (!strcmp(argv[i], "--rerecord")) g_rerecord = 1;

	struct gate_opts o;
	if (!gate_parse_opts(argc, argv, 3, &o))
		return 2;

	FILE *wf = fopen(wbxPath, "rb");
	if (!wf) { perror(wbxPath); return 1; }

	/* matches waterbox.config memoryLayoutMiB. A Dreamcast is BIG: 16MB of
	 * system RAM, 16MB of video RAM, 8MB for the AICA and 32MB for the Elan -
	 * about 72MB of machine before Flycast has allocated anything of its own,
	 * and with no virtual memory those are ordinary allocations inside the
	 * guest's heap. Sizing this like an 8-bit core is how the first sandboxed
	 * run died: an allocation quietly failed and the machine wrote through a
	 * null base. */
	mb_memory_layout_template layout = { 256u << 20, 16u << 20, 64u << 20, 16u << 20, 256u << 20 };
	freader fr = { wf };
	mb_return r;
	wbx_create_host(&layout, "core.wbx", file_read, (uintptr_t)&fr, &r);
	fclose(wf);
	if (r.error_message[0]) { fprintf(stderr, "create: %s\n", r.error_message); return 1; }
	g_host = (mb_host *)r.data;

	/* mount the whole work dir: rom under its real name + slots + settings.
	 * The sol movie may live in the same dir; it is the driver's, not the
	 * guest's, so skip it (and any .tga the screenshot option wrote). */
	DIR *d = opendir(workdir);
	if (!d) { perror(workdir); return 1; }
	struct dirent *de;
	while ((de = readdir(d)) != NULL)
	{
		char path[4096];
		snprintf(path, sizeof path, "%s/%s", workdir, de->d_name);
		struct stat st;
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
			continue;
		const char *dot = strrchr(de->d_name, '.');
		if (dot && (!strcmp(dot, ".sol") || !strcmp(dot, ".tga") || !strcmp(dot, ".txt")))
			continue;
		FILE *f = fopen(path, "rb");
		if (!f) { perror(path); return 1; }
		freader rd = { f };
		wbx_mount_file(g_host, de->d_name, file_read, (uintptr_t)&rd, false, &r);
		fclose(f);
		if (r.error_message[0]) { fprintf(stderr, "mount %s: %s\n", de->d_name, r.error_message); return 1; }
	}
	closedir(d);

	wbx_activate_host(g_host, &r);

	g_Init = (intfn)proc(g_host, "Init");
	g_GetLoadError = (ptrfn)proc(g_host, "GetLoadError");
	g_SetButton = (setfn)proc(g_host, "SetButton");
	g_SetAxis = (setfn)proc(g_host, "SetAxis");
	g_FrameAdvance = (framefn)proc(g_host, "FrameAdvance");
	g_GetVideoBgra = (ptrfn)proc(g_host, "GetVideoBgra");
	g_GetVideoWidth = (intfn)proc(g_host, "GetVideoWidth");
	g_GetVideoHeight = (intfn)proc(g_host, "GetVideoHeight");
	g_GetAudio = (ptrfn)proc(g_host, "GetAudio");
	g_GetAudioSampleCount = (intfn)proc(g_host, "GetAudioSampleCount");
	g_InputWasRead = (intfn)proc(g_host, "InputWasRead");
	g_GetMemoryDomainCount = (intfn)proc(g_host, "GetMemoryDomainCount");
	g_GetMemoryDomainName = (ptrfn_i)proc(g_host, "GetMemoryDomainName");
	g_GetMemoryDomainPtr = (ptrfn_i)proc(g_host, "GetMemoryDomainPtr");
	g_GetMemoryDomainSize = (i64fn_i)proc(g_host, "GetMemoryDomainSize");
	g_GetVsyncNumerator = (intfn)proc(g_host, "GetVsyncNumerator");
	g_GetVsyncDenominator = (intfn)proc(g_host, "GetVsyncDenominator");
	g_GetSaveDataFileCount = (i32fn)proc(g_host, "GetSaveDataFileCount");
	g_GetSaveDataFileName = (ptrfn_i32)proc(g_host, "GetSaveDataFileName");
	g_GetSaveDataFileSize = (i64fn_i32)proc(g_host, "GetSaveDataFileSize");
	g_GetSaveDataFileBuffer = (ptrfn_i32)proc(g_host, "GetSaveDataFileBuffer");

	struct gate_core c = {
		.init = core_init,
		.load_error = core_load_error,
		.set_button = core_set_button,
		.frame = core_frame,
		.video = core_video,
		.audio = core_audio,
		.input_was_read = core_input_was_read,
		.domain_count = core_domain_count,
		.domain_name = core_domain_name,
		.domain_ptr = core_domain_ptr,
		.domain_size = core_domain_size,
		.set_axis = core_set_axis,
		.vsync_numerator = core_vsync_numerator,
		.vsync_denominator = core_vsync_denominator,
		.savedata_count = core_savedata_count,
		.savedata_name = core_savedata_name,
		.savedata_size = core_savedata_size,
		.savedata_buffer = core_savedata_buffer,
		.pre_frame = core_pre_frame,
	};

	/* Init runs before Seal - the loaded machine is the sealed baseline */
	if (c.init() != 1)
	{
		fprintf(stderr, "Init failed: %s\n", c.load_error());
		return 1;
	}

	wbx_deactivate_host(g_host, &r);
	wbx_seal(g_host, &r);
	if (r.error_message[0]) { fprintf(stderr, "seal: %s\n", r.error_message); return 1; }
	wbx_activate_host(g_host, &r);

	c.init = core_init_done;
	int ret = gate_run(&c, &o);

	wbx_deactivate_host(g_host, &r);
	wbx_destroy_host(g_host, &r);
	free(g_state.b);
	return ret;
}
