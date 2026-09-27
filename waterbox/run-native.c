/* run-native.c - the native reference for the equivalence gate.
 *
 * Links the SAME driver, exports and Azahar libraries the guest build uses
 * (emulibc degraded to malloc by native-shim/) and drives the exports
 * directly. The work dir holds the same files the sandbox would see mounted:
 * the game and any firmware under their names, plus the "slots" and
 * "settings" JSON.
 *
 * usage: run-native <workdir> [options - see gate-harness.h]
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include "gate-harness.h"

extern int Init(void);
extern const char *GetLoadError(void);
extern void SetButton(int32_t index, int32_t state);
extern int IsButtonActive(int32_t index);
extern const char *GetButtonName(int32_t index);
extern int GetButtonCount(void);
extern int32_t GetSaveDataFileCount(void);
extern const char *GetSaveDataFileName(int32_t i);
extern int64_t GetSaveDataFileSize(int32_t i);
extern const uint8_t *GetSaveDataFileBuffer(int32_t i);
static int save_count(void) { return GetSaveDataFileCount(); }
static const char *save_name(int i) { return GetSaveDataFileName(i); }
static int64_t save_size(int i) { return GetSaveDataFileSize(i); }
static const uint8_t *save_data(int i) { return GetSaveDataFileBuffer(i); }
extern void FrameAdvance(uint64_t packed);
extern int InputWasRead(void);
extern int IsAxisActive(int32_t index);
extern void SetAxis(int32_t index, int32_t value);
extern int GetAxisCount(void);
extern uint32_t *GetVideoBgra(void);
extern int GetVideoWidth(void);
extern int GetVideoHeight(void);
extern int16_t *GetAudio(void);
extern int GetAudioSampleCount(void);
extern int GetMemoryDomainCount(void);
extern const char *GetMemoryDomainName(int i);
extern uint8_t *GetMemoryDomainPtr(int i);
extern int64_t GetMemoryDomainSize(int i);

static void frame(void) { FrameAdvance(0); }
static const uint32_t *video(int *w, int *h)
{
	*w = GetVideoWidth();
	*h = GetVideoHeight();
	return GetVideoBgra();
}
static const int16_t *audio(int *n)
{
	*n = GetAudioSampleCount();
	return GetAudio();
}
static const uint8_t *domain_ptr(int i) { return GetMemoryDomainPtr(i); }

int main(int argc, char **argv)
{
	if (argc < 2)
	{
		fprintf(stderr, "usage: run-native <workdir> [options]\n");
		return 2;
	}
	if (chdir(argv[1]) != 0)
	{
		perror(argv[1]);
		return 1;
	}
	struct gate_opts o;
	if (!gate_parse_opts(argc, argv, 2, &o))
		return 2;
	struct gate_core c = {
		.init = Init,
		.load_error = GetLoadError,
		.set_button = SetButton,
		.button_count = GetButtonCount,
		.button_active = IsButtonActive,
		.button_name = GetButtonName,
		.frame = frame,
		.input_was_read = InputWasRead,
		.axis_count = GetAxisCount,
		.axis_active = IsAxisActive,
		.set_axis = SetAxis,
		.video = video,
		.audio = audio,
		.domain_count = GetMemoryDomainCount,
		.domain_name = GetMemoryDomainName,
		.domain_ptr = domain_ptr,
		.domain_size = GetMemoryDomainSize,
		.pre_frame = NULL,
		.save_count = save_count,
		.save_name = save_name,
		.save_size = save_size,
		.save_data = save_data,
	};
	const int ret = gate_run(&c, &o);
	/* The machine is never torn down: a process that ends is the end of it,
	 * and Azahar's singletons do not survive their static destructors. */
	fflush(stdout);
	fflush(stderr);
	_exit(ret);
}
