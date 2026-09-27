/* gate-harness.h - the run both harnesses share: run-native (the exports
 * compiled for the host) and run-wbx (core.wbx in the sandbox) parse the same
 * options, press the same controls on the same frames and print the same
 * digests, so their outputs diff directly. A difference between the flavors
 * is then the sandbox's doing, never the harness's.
 *
 * options: --frames N          run length (default 60)
 *          --report N          a digest line every N frames, and the last
 *          --press I:FIRST:N   hold panel control I for N frames from FIRST
 *          --axis I:V:FIRST:N  hold axis I at V for N frames from FIRST, then
 *                              put it back to its neutral
 *          --exercise          a deterministic wander over the live buttons,
 *                              the sticks and the touch panel
 *          --list-panel        print the panel, and which controls are live
 *          --dump-domain NAME FILE   write a memory domain after the run
 *          --vid-out FILE      a picture: "W H\n" then BGRA rows (the last
 *                              frame, or the frame --vid-at names)
 *          --vid-at F          take --vid-out's picture after frame F
 *          --savedata-out DIR  the save data after the run, a file each
 * SPDX-License-Identifier: MIT
 */
#ifndef GATE_HARNESS_H
#define GATE_HARNESS_H

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct gate_core
{
	int (*init)(void);
	const char *(*load_error)(void);
	void (*set_button)(int32_t index, int32_t state);
	int (*button_count)(void);
	int (*button_active)(int32_t index);
	const char *(*button_name)(int32_t index);
	void (*frame)(void);
	int (*input_was_read)(void);
	int (*axis_count)(void);
	int (*axis_active)(int32_t index);
	void (*set_axis)(int32_t index, int32_t value);
	const uint32_t *(*video)(int *w, int *h);
	const int16_t *(*audio)(int *n);
	int (*domain_count)(void);
	const char *(*domain_name)(int i);
	const uint8_t *(*domain_ptr)(int i);
	int64_t (*domain_size)(int i);
	void (*pre_frame)(void); /* run-wbx's rerecord / session hook, or NULL */
	int (*save_count)(void);
	const char *(*save_name)(int i);
	int64_t (*save_size)(int i);
	const uint8_t *(*save_data)(int i);
};

/* The panel's axes (waterbox.config): Circle Pad X/Y and C-Stick X/Y are
 * -128..127 about 0, Touch X/Y 0..65535 about 32768, the accelerometer in
 * thousandths of a g (Z rests at -1000) and the gyroscope in tenths of a
 * degree a second. */
static const int32_t gate_axis_min[12] = {-128, -128, -128, -128, 0, 0, -4000, -4000, -4000, -20000, -20000, -20000};
static const int32_t gate_axis_max[12] = {127, 127, 127, 127, 65535, 65535, 4000, 4000, 4000, 20000, 20000, 20000};
static const int32_t gate_axis_neutral[12] = {0, 0, 0, 0, 32768, 32768, 0, 0, -1000, 0, 0, 0};

#define GATE_MAX_PRESS 32
struct gate_opts
{
	long frames;
	long report;
	int presses;
	struct { int index; long first, count; } press[GATE_MAX_PRESS];
	int axes;
	struct { int index, value; long first, count; } axis[GATE_MAX_PRESS];
	int exercise;
	int listPanel;
	const char *dumpDomain, *dumpPath;
	const char *vidOut;
	long vidAt;
	const char *savedataOut;
};

static uint64_t gate_fnv(uint64_t h, const void *p, size_t n)
{
	const uint8_t *b = (const uint8_t *)p;
	if (!h) h = 1469598103934665603ULL;
	for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
	return h;
}

/* A digest of a big domain: 64-bit words, not bytes - FCRAM is 256 MiB and
 * the byte-wise FNV of it costs more than the frame it describes. */
static uint64_t gate_hash_words(uint64_t h, const void *p, size_t n)
{
	const uint64_t *w = (const uint64_t *)p;
	if (!h) h = 1469598103934665603ULL;
	for (size_t i = 0; i < n / 8; i++) { h ^= w[i]; h *= 1099511628211ULL; h ^= h >> 29; }
	return gate_fnv(h, (const uint8_t *)p + (n & ~(size_t)7), n & 7);
}

static int gate_parse_opts(int argc, char **argv, int first, struct gate_opts *o)
{
	memset(o, 0, sizeof *o);
	o->frames = 60;
	o->report = 10;
	o->vidAt = -1;
	for (int i = first; i < argc; i++)
	{
		if (!strcmp(argv[i], "--frames") && i + 1 < argc) o->frames = strtol(argv[++i], 0, 0);
		else if (!strcmp(argv[i], "--report") && i + 1 < argc) o->report = strtol(argv[++i], 0, 0);
		else if (!strcmp(argv[i], "--press") && i + 1 < argc && o->presses < GATE_MAX_PRESS)
		{
			int idx; long f, c;
			if (sscanf(argv[++i], "%d:%ld:%ld", &idx, &f, &c) != 3)
			{
				fprintf(stderr, "--press wants INDEX:FIRST:COUNT\n");
				return 0;
			}
			o->press[o->presses].index = idx;
			o->press[o->presses].first = f;
			o->press[o->presses].count = c;
			o->presses++;
		}
		else if (!strcmp(argv[i], "--axis") && i + 1 < argc && o->axes < GATE_MAX_PRESS)
		{
			int idx, val; long f, c;
			if (sscanf(argv[++i], "%d:%d:%ld:%ld", &idx, &val, &f, &c) != 4)
			{
				fprintf(stderr, "--axis wants INDEX:VALUE:FIRST:COUNT\n");
				return 0;
			}
			o->axis[o->axes].index = idx;
			o->axis[o->axes].value = val;
			o->axis[o->axes].first = f;
			o->axis[o->axes].count = c;
			o->axes++;
		}
		else if (!strcmp(argv[i], "--exercise")) o->exercise = 1;
		else if (!strcmp(argv[i], "--list-panel")) o->listPanel = 1;
		else if (!strcmp(argv[i], "--dump-domain") && i + 2 < argc) { o->dumpDomain = argv[++i]; o->dumpPath = argv[++i]; }
		else if (!strcmp(argv[i], "--vid-out") && i + 1 < argc) o->vidOut = argv[++i];
		else if (!strcmp(argv[i], "--vid-at") && i + 1 < argc) o->vidAt = strtol(argv[++i], 0, 0);
		else if (!strcmp(argv[i], "--savedata-out") && i + 1 < argc) o->savedataOut = argv[++i];
		else if (!strcmp(argv[i], "--rerecord") || !strcmp(argv[i], "--session")) ; /* run-wbx's */
		else
		{
			fprintf(stderr, "unknown option %s\n", argv[i]);
			return 0;
		}
	}
	if (o->report <= 0) o->report = o->frames;
	return 1;
}

/* The exercise: an LCG picks, every 6 frames, one live button and holds it
 * for 3. Touch is a button too, so a long exercise touches the bottom screen
 * wherever the Touch axes have wandered to. */
static void gate_exercise(const struct gate_core *c, long frame, uint8_t *held)
{
	static uint32_t seed = 12345;
	const int count = c->button_count();
	if (frame % 6 == 0)
	{
		memset(held, 0, 64);
		seed = seed * 1103515245u + 12345u;
		for (int tries = 0; tries < count; tries++)
		{
			const int i = (int)((seed >> 8) % (uint32_t)count);
			seed = seed * 1103515245u + 12345u;
			if (c->button_active(i)) { held[i] = 1; break; }
		}
	}
	else if (frame % 6 == 3)
		memset(held, 0, 64);
}

static uint64_t gate_ram_hash(const struct gate_core *c)
{
	uint64_t h = 0;
	for (int i = 0; i < c->domain_count(); i++)
		h = gate_hash_words(h, c->domain_ptr(i), (size_t)c->domain_size(i));
	return h;
}

static void gate_write_video(const struct gate_core *c, const char *path)
{
	int w, h;
	const uint32_t *v = c->video(&w, &h);
	FILE *f = fopen(path, "wb");
	if (f) { fprintf(f, "%d %d\n", w, h); fwrite(v, 4, (size_t)w * h, f); fclose(f); }
}

static void gate_mkdirs(char *path)
{
	for (char *p = path + 1; *p; p++)
		if (*p == '/') { *p = 0; mkdir(path, 0755); *p = '/'; }
}

static int gate_run(const struct gate_core *c, const struct gate_opts *o)
{
	if (c->init() != 1)
	{
		fprintf(stderr, "Init failed: %s\n", c->load_error());
		return 1;
	}
	const int count = c->button_count();
	if (o->listPanel)
	{
		for (int i = 0; i < count; i++)
			printf("panel %d '%s' %s\n", i, c->button_name(i), c->button_active(i) ? "active" : "-");
		for (int a = 0; a < c->axis_count(); a++)
			printf("axis %d %s\n", a, c->axis_active(a) ? "active" : "-");
	}
	uint8_t held[64] = {0};
	long lag = 0;
	for (long f = 1; f <= o->frames; f++)
	{
		if (c->pre_frame) c->pre_frame();
		if (o->exercise) gate_exercise(c, f, held);
		/* the exercise moves every live axis too: a slow sweep over each
		 * axis's own range, the same in both flavors */
		if (o->exercise)
			for (int a = 0; a < c->axis_count() && a < 12; a++)
				if (c->axis_active(a))
				{
					const int64_t span = (int64_t)gate_axis_max[a] - gate_axis_min[a] + 1;
					c->set_axis(a, (int32_t)(gate_axis_min[a] + (f * 37 + a * 311) * span / 2048 % span));
				}
		for (int i = 0; i < count && i < 64; i++)
		{
			int on = held[i];
			for (int p = 0; p < o->presses; p++)
				if (o->press[p].index == i && f >= o->press[p].first && f < o->press[p].first + o->press[p].count)
					on = 1;
			c->set_button(i, on);
		}
		for (int a = 0; a < o->axes; a++)
		{
			const long first = o->axis[a].first, n = o->axis[a].count;
			const int idx = o->axis[a].index;
			if (f == first) c->set_axis(idx, o->axis[a].value);
			if (f == first + n) c->set_axis(idx, idx >= 0 && idx < 12 ? gate_axis_neutral[idx] : 0);
		}
		c->frame();
		if (!c->input_was_read()) lag++;
		if (f % o->report == 0 || f == o->frames)
		{
			int w, h, n;
			const uint32_t *v = c->video(&w, &h);
			const int16_t *a = c->audio(&n);
			printf("frame %5ld ram %016" PRIx64 " vid %dx%d %016" PRIx64 " aud %d %016" PRIx64 " lag %ld\n", f,
			       gate_ram_hash(c), w, h, gate_fnv(0, v, (size_t)w * h * 4), n,
			       gate_fnv(0, a, (size_t)n * 4), lag);
			fflush(stdout);
		}
		if (o->vidOut && f == o->vidAt)
			gate_write_video(c, o->vidOut);
	}
	if (o->dumpDomain)
	{
		int found = 0;
		for (int i = 0; i < c->domain_count(); i++)
			if (!strcmp(c->domain_name(i), o->dumpDomain))
			{
				FILE *f = fopen(o->dumpPath, "wb");
				if (f) { fwrite(c->domain_ptr(i), 1, (size_t)c->domain_size(i), f); fclose(f); }
				found = 1;
			}
		if (!found) { fprintf(stderr, "no memory domain '%s'\n", o->dumpDomain); return 1; }
	}
	if (o->savedataOut)
		for (int i = 0; i < c->save_count(); i++)
		{
			char path[2048];
			snprintf(path, sizeof path, "%s/%s", o->savedataOut, c->save_name(i));
			gate_mkdirs(path);
			FILE *f = fopen(path, "wb");
			if (f) { fwrite(c->save_data(i), 1, (size_t)c->save_size(i), f); fclose(f); }
		}
	if (o->vidOut && o->vidAt < 0)
		gate_write_video(c, o->vidOut);
	return 0;
}

#endif
