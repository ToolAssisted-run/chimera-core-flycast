/* gate-harness.h - the shared half of run-native.c and run-wbx.c.
 *
 * One replay-and-digest loop over an abstract core interface, so the native
 * reference and the sandboxed core run EXACTLY the same schedule: the same
 * .sol movie (quickerSnes9x's format) or the same pseudo-random pad exercise,
 * digesting video, audio, lag and every memory domain per frame. The two
 * drivers differ only in how the exports are reached.
 *
 * Wire format (waterbox.config button order): 0 Power, 1 Reset, 2 Select,
 * 3 Left Difficulty, 4 Right Difficulty, 5 TV Type, then P1 and P2 x
 * {Up,Down,Left,Right,Button}. quickerStella's .sol writes the console field
 * in its own order; gate_parse_line maps it.
 */
#ifndef GATE_HARNESS_H
#define GATE_HARNESS_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the wire this core declares: Power, the four console switches plus TV type,
 * and two ports of five buttons (waterbox.config "input.buttons") */
#define GATE_BTN_COUNT 16

struct gate_core
{
	int (*init)(void);
	const char *(*load_error)(void);
	void (*set_button)(int32_t index, int32_t state);
	void (*frame)(void);
	const uint32_t *(*video)(int *w, int *h);
	const int16_t *(*audio)(int *n);
	int (*input_was_read)(void);
	int (*domain_count)(void);
	const char *(*domain_name)(int i);
	const uint8_t *(*domain_ptr)(int i);
	int64_t (*domain_size)(int i);
	void (*set_axis)(int32_t index, int32_t value); /* NULL if the core has no axes */
	int (*vsync_numerator)(void);
	int (*vsync_denominator)(void);
	int32_t (*savedata_count)(void);
	const char *(*savedata_name)(int32_t i);
	int64_t (*savedata_size)(int32_t i);
	const uint8_t *(*savedata_buffer)(int32_t i);
	/* optional per-frame hook (the rerecord leg); may be NULL */
	void (*pre_frame)(void);
};

struct gate_opts
{
	long frames;          /* run length; a movie may end earlier padded with idle */
	const char *solPath;  /* NULL = pad-exercise schedule */
	const char *sys;      /* only "snes" exists; kept for CLI symmetry */
	const char *ctl1;     /* none | joypad */
	const char *ctl2;
	const char *screenshotPath; /* optional final-frame .tga */
	int exercise;         /* nonzero: drive P1 with a deterministic pattern */
	const char *dumpDomain;     /* optional: memory domain to dump after the run... */
	const char *dumpPath;       /* ...into this file (the frontend gate compares it) */
	const char *savedataDir;    /* optional: write every savedata export here after the run */
	int exercisePad;      /* also drive this pad number (2..8) with the exercise, or 0 */
	int wiggleAxes;       /* nonzero: drive every axis with a deterministic wander */
};

static uint64_t gate_fnv(uint64_t h, const void *p, size_t n)
{
	const uint8_t *b = (const uint8_t *)p;
	if (!h) h = 1469598103934665603ULL;
	for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
	return h;
}

/* deterministic P1 pad pattern, identical in both drivers, so the input
 * path is part of the comparison; Power/Reset stay untouched */
static void gate_exercise_pad(long frame, uint8_t *buttons)
{
	uint64_t x = (uint64_t)frame * 6364136223846793005ULL + 1442695040888963407ULL;
	x ^= x >> 33;
	/* the console switches (1..5) and player one's pad (6..10); POWER is left
	 * alone, since resetting the machine every few frames proves nothing */
	for (int k = 0; k < 10; k++)
		buttons[1 + k] = (x >> k) & 1;
	/* holding UP+DOWN or LEFT+RIGHT is not a pad state a real controller
	 * produces and some games misbehave; drop the contradictions */
	if (buttons[6] && buttons[7]) buttons[7] = 0;
	if (buttons[8] && buttons[9]) buttons[9] = 0;
}

/* ---- .sol parsing (quickerGPGX's movie format) ---- */

struct gate_sol
{
	char **lines;
	long count;
};

static int gate_sol_load(const char *path, struct gate_sol *sol)
{
	FILE *f = fopen(path, "rb");
	if (!f) return 0;
	sol->lines = NULL;
	sol->count = 0;
	long cap = 0;
	char line[256];
	while (fgets(line, sizeof line, f))
	{
		size_t n = strlen(line);
		while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
		if (n == 0) continue;
		if (sol->count == cap)
		{
			cap = cap ? cap * 2 : 1024;
			sol->lines = (char **)realloc(sol->lines, (size_t)cap * sizeof(char *));
		}
		sol->lines[sol->count++] = strdup(line);
	}
	fclose(f);
	return 1;
}

/* one quickerStella joypad field -> the wire's pad block at base; the columns
 * U D L R B map 1:1 onto the wire order. A 2600 pad is four directions and one
 * button, and a driving controller uses the same columns (its rotation is the
 * left and right ones), so both parse the same way. */
static int gate_parse_pad(const char *ctl, const char *s, uint8_t *buttons, int base)
{
	static const char cols[5] = { 'U','D','L','R','B' };
	if (strcmp(ctl, "none") == 0)
		return 0;
	if (strcmp(ctl, "joypad") != 0 && strcmp(ctl, "joystick") != 0 && strcmp(ctl, "driving") != 0)
		return -1;
	for (int i = 0; i < 5; i++)
	{
		if (s[i] != '.' && s[i] != cols[i])
			return -1;
		buttons[base + i] = s[i] != '.';
	}
	return 5;
}

/* a full |r s P l r|U D L R B|... line -> wire buttons; 0 on malformed input.
 *
 * quickerStella's console field is the machine's five switches in ITS order -
 * reset, select, power, left difficulty, right difficulty - which is not the
 * wire's order (power first, then the switches, then TV type). The mapping is
 * spelled out here rather than assumed, because a movie that decodes into the
 * wrong switches would still replay, just differently.
 */
static int gate_parse_line(const char *line, const char *sys, const char *ctl1,
	const char *ctl2, uint8_t *buttons)
{
	(void)sys;
	memset(buttons, 0, GATE_BTN_COUNT);
	const char *s = line;
	if (*s++ != '|') return 0;

	/* movie column -> wire index; the wire's TV type has no movie column */
	static const char consoleCols[5] = { 'r', 's', 'P', 'l', 'r' };
	static const int consoleWire[5] = { 1, 2, 0, 3, 4 };
	for (int i = 0; i < 5; i++)
	{
		if (s[i] != '.' && s[i] != consoleCols[i]) return 0;
		buttons[consoleWire[i]] = s[i] != '.';
	}
	s += 5;

	int n;
	if (strcmp(ctl1, "none") != 0)
	{
		if (*s++ != '|') return 0;
		n = gate_parse_pad(ctl1, s, buttons, 6);
		if (n < 0) return 0;
		s += n;
	}
	if (strcmp(ctl2, "none") != 0)
	{
		if (*s++ != '|') return 0;
		n = gate_parse_pad(ctl2, s, buttons, 11);
		if (n < 0) return 0;
		s += n;
	}
	if (*s++ != '|') return 0;

	return *s == 0;
}

static int gate_write_tga(const char *path, const uint32_t *bgra, int w, int h)
{
	FILE *f = fopen(path, "wb");
	if (!f) return 0;
	uint8_t hdr[18] = { 0 };
	hdr[2] = 2;
	hdr[12] = w & 0xff; hdr[13] = (w >> 8) & 0xff;
	hdr[14] = h & 0xff; hdr[15] = (h >> 8) & 0xff;
	hdr[16] = 32;
	hdr[17] = 0x20;
	fwrite(hdr, 1, 18, f);
	fwrite(bgra, 4, (size_t)w * h, f);
	fclose(f);
	return 1;
}

/* the loop both drivers share; prints the digest block to stdout */
static int gate_run(const struct gate_core *c, const struct gate_opts *o)
{
	struct gate_sol sol = { NULL, 0 };
	if (o->solPath && !gate_sol_load(o->solPath, &sol))
	{
		fprintf(stderr, "cannot read movie %s\n", o->solPath);
		return 1;
	}

	if (c->init() != 1)
	{
		fprintf(stderr, "Init failed: %s\n", c->load_error ? c->load_error() : "?");
		return 1;
	}

	long frames = o->frames;
	if (sol.count > 0 && frames < sol.count)
		frames = sol.count;

	uint64_t vh = 0, ah = 0;
	long lag = 0;
	uint8_t buttons[GATE_BTN_COUNT];
	uint8_t prev[GATE_BTN_COUNT];
	memset(prev, 0, sizeof prev);

	for (long f = 0; f < frames; f++)
	{
		memset(buttons, 0, sizeof buttons);
		if (f < sol.count)
		{
			if (!gate_parse_line(sol.lines[f], o->sys, o->ctl1, o->ctl2, buttons))
			{
				fprintf(stderr, "bad movie line %ld: '%s'\n", f, sol.lines[f]);
				return 1;
			}
		}
		else if (o->exercise)
		{
			gate_exercise_pad(f, buttons);
			if (o->exercisePad >= 2 && o->exercisePad <= 8)
			{
				uint8_t extra[GATE_BTN_COUNT];
				memset(extra, 0, sizeof extra);
				gate_exercise_pad(f + 7919, extra); /* a shifted schedule */
				memcpy(&buttons[2 + (o->exercisePad - 1) * 12], &extra[2], 12);
			}
		}

		if (o->wiggleAxes && c->set_axis)
		{
			/* a deterministic wander: mouse deltas -2..2, gun coords circling
			 * the middle of the screen; the same values in both drivers */
			uint64_t w = (uint64_t)f * 2862933555777941757ULL + 3037000493ULL;
			w ^= w >> 29;
			c->set_axis(0, (int32_t)(w % 5) - 2);
			c->set_axis(1, (int32_t)((w >> 8) % 5) - 2);
			c->set_axis(2, 96 + (int32_t)((w >> 16) % 64));
			c->set_axis(3, 88 + (int32_t)((w >> 24) % 64));
			c->set_axis(4, 96 + (int32_t)((w >> 32) % 64));
			c->set_axis(5, 88 + (int32_t)((w >> 40) % 64));
			/* the device buttons (wire 98..107) ride the same wander */
			for (int k = 98; k < GATE_BTN_COUNT; k++)
				buttons[k] = (w >> (48 + (k - 98))) & 1;
		}

		if (c->pre_frame)
			c->pre_frame();

		for (int i = 0; i < GATE_BTN_COUNT; i++)
		{
			if (buttons[i] != prev[i])
				c->set_button(i, buttons[i]);
			prev[i] = buttons[i];
		}

		c->frame();

		int w = 0, h = 0, n = 0;
		const uint32_t *video = c->video(&w, &h);
		const int16_t *audio = c->audio(&n);
		vh = gate_fnv(vh, &w, sizeof w);
		vh = gate_fnv(vh, &h, sizeof h);
		vh = gate_fnv(vh, video, (size_t)w * h * 4);
		ah = gate_fnv(ah, audio, (size_t)n * 2 * sizeof(int16_t));
		if (!c->input_was_read())
			lag++;
		if (o->screenshotPath && f == frames - 1)
			gate_write_tga(o->screenshotPath, video, w, h);
	}

	printf("frames=%ld\n", frames);
	printf("vsync=%d/%d\n", c->vsync_numerator(), c->vsync_denominator());
	printf("videoHash=%016llx\n", (unsigned long long)vh);
	printf("audioHash=%016llx\n", (unsigned long long)ah);
	printf("lagFrames=%ld\n", lag);
	int nd = c->domain_count();
	for (int i = 0; i < nd; i++)
	{
		uint64_t dh = gate_fnv(0, c->domain_ptr(i), (size_t)c->domain_size(i));
		printf("domain[%s]=%016llx\n", c->domain_name(i), (unsigned long long)dh);
	}
	if (o->savedataDir)
	{
		int32_t count = c->savedata_count();
		printf("savedataFiles=%d\n", count);
		for (int32_t i = 0; i < count; i++)
		{
			char path[4096];
			snprintf(path, sizeof path, "%s/%s", o->savedataDir, c->savedata_name(i));
			FILE *f = fopen(path, "wb");
			if (!f) { perror(path); return 1; }
			fwrite(c->savedata_buffer(i), 1, (size_t)c->savedata_size(i), f);
			fclose(f);
		}
	}

	if (o->dumpDomain && o->dumpPath)
	{
		int found = 0;
		for (int i = 0; i < nd; i++)
		{
			if (strcmp(c->domain_name(i), o->dumpDomain) != 0)
				continue;
			FILE *f = fopen(o->dumpPath, "wb");
			if (!f) { perror(o->dumpPath); return 1; }
			fwrite(c->domain_ptr(i), 1, (size_t)c->domain_size(i), f);
			fclose(f);
			found = 1;
			break;
		}
		if (!found)
		{
			fprintf(stderr, "no such domain to dump: %s\n", o->dumpDomain);
			return 1;
		}
	}
	return 0;
}

/* shared CLI parsing: returns the index of the first positional argument's
 * slot usage message on error */
static int gate_parse_opts(int argc, char **argv, int first, struct gate_opts *o)
{
	o->frames = 600;
	o->solPath = NULL;
	o->sys = "snes";
	o->ctl1 = "joypad";
	o->ctl2 = "none";
	o->screenshotPath = NULL;
	o->exercise = 0;
	o->dumpDomain = NULL;
	o->dumpPath = NULL;
	o->savedataDir = NULL;
	o->exercisePad = 0;
	o->wiggleAxes = 0;
	for (int i = first; i < argc; i++)
	{
		if (!strcmp(argv[i], "--frames") && i + 1 < argc) o->frames = strtol(argv[++i], 0, 0);
		else if (!strcmp(argv[i], "--sol") && i + 1 < argc) o->solPath = argv[++i];
		else if (!strcmp(argv[i], "--sys") && i + 1 < argc) o->sys = argv[++i];
		else if (!strcmp(argv[i], "--ctl1") && i + 1 < argc) o->ctl1 = argv[++i];
		else if (!strcmp(argv[i], "--ctl2") && i + 1 < argc) o->ctl2 = argv[++i];
		else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) o->screenshotPath = argv[++i];
		else if (!strcmp(argv[i], "--exercise")) o->exercise = 1;
		else if (!strcmp(argv[i], "--dump-domain") && i + 2 < argc) { o->dumpDomain = argv[++i]; o->dumpPath = argv[++i]; }
		else if (!strcmp(argv[i], "--savedata-out") && i + 1 < argc) o->savedataDir = argv[++i];
		else if (!strcmp(argv[i], "--exercise-pad") && i + 1 < argc) o->exercisePad = (int)strtol(argv[++i], 0, 0);
		else if (!strcmp(argv[i], "--wiggle-axes")) o->wiggleAxes = 1;
		else if (!strcmp(argv[i], "--rerecord")) ; /* run-wbx's; ignored here */
		else { fprintf(stderr, "unknown argument %s\n", argv[i]); return 0; }
	}
	return 1;
}

#endif /* GATE_HARNESS_H */
