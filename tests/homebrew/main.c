/* chimera-test: a 3DS program for the core's test script (chimera issue 260).
 *
 * A public CI runner may not hold a commercial game, so the script boots this
 * instead. It is written for the test and asks the machine for three things a
 * game does every frame:
 *
 *   - the top screen: a colour ramp with a band that moves one column a frame,
 *     so no two frames in a stretch are the same picture;
 *   - the pad: what is held, kept in memory and shown as the bottom screen's
 *     colour, so a run that presses buttons is another machine than one that
 *     does not;
 *   - the frame: it waits for each vertical blank, so it runs at the
 *     console's own pace.
 *
 * Once, before the first frame, it also commits save data and times how long
 * the console kept it waiting (chimera issue 345).
 *
 * It uses libctru (zlib licence) and nothing else, and is built with
 * devkitARM by the Makefile beside it.
 */
#include <3ds.h>

/* what the pad held, every frame: in memory, where the test script's digest
 * of the machine sees it */
volatile u32 g_held_history[64];
volatile u32 g_frames;

/* one save-data commit, timed (chimera issue 345): the test script finds this
 * in the console's memory by its tag and reads what the commit answered and
 * how many console ticks it kept the program waiting */
volatile struct
{
	char tag[12];
	u32 result;
	u32 ticks;
} g_commit = {"COMMITPROBE", 0xffffffff, 0};

static void time_a_commit(void)
{
	FS_Archive sdmc;
	if (R_FAILED(FSUSER_OpenArchive(&sdmc, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""))))
		return;
	const u64 before = svcGetSystemTick();
	g_commit.result = FSUSER_ControlArchive(sdmc, ARCHIVE_ACTION_COMMIT_SAVE_DATA, NULL, 0, NULL, 0);
	g_commit.ticks = (u32)(svcGetSystemTick() - before);
	FSUSER_CloseArchive(sdmc);
}

static void fill_top(u8 *fb, u16 w, u16 h, u32 frame)
{
	/* the framebuffer is the screen turned: w is its height (240), h its
	 * width (400), three bytes a pixel, blue first */
	for (u16 x = 0; x < h; x++)
	{
		const int band = ((x + 400 - (frame % 400)) % 400) < 16;
		for (u16 y = 0; y < w; y++)
		{
			u8 *p = fb + 3 * (x * w + y);
			p[0] = band ? 255 : (u8)(y);
			p[1] = band ? 255 : (u8)(x / 2);
			p[2] = band ? 255 : (u8)(frame);
		}
	}
}

static void fill_bottom(u8 *fb, u16 w, u16 h, u32 held)
{
	const u8 b = (held & KEY_A) ? 255 : 32;
	const u8 g = (held & (KEY_B | KEY_DUP | KEY_DDOWN)) ? 255 : 32;
	const u8 r = (held & (KEY_X | KEY_Y | KEY_START)) ? 255 : 32;
	for (u32 i = 0; i < (u32)w * h; i++)
	{
		fb[3 * i] = b;
		fb[3 * i + 1] = g;
		fb[3 * i + 2] = r;
	}
}

int main(void)
{
	gfxInitDefault();
	gfxSetDoubleBuffering(GFX_TOP, true);
	gfxSetDoubleBuffering(GFX_BOTTOM, true);
	time_a_commit();
	while (aptMainLoop())
	{
		hidScanInput();
		const u32 held = hidKeysHeld();
		g_held_history[g_frames % 64] = held;
		u16 w, h;
		fill_top(gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &w, &h), w, h, g_frames);
		fill_bottom(gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &w, &h), w, h, held);
		gfxFlushBuffers();
		gfxSwapBuffers();
		gspWaitForVBlank();
		g_frames++;
	}
	gfxExit();
	return 0;
}
