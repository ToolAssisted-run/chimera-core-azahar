# chimera-core-azahar: plan and state

Azahar (the Citra fork) as a Chimera core, for chimera#155. The evaluation
behind it (2026-09-27) recommended going ahead on four conditions, all kept:

1. no Nintendo key material in the package (`ENABLE_BUILTIN_KEYBLOB=OFF`);
   `aes_keys.txt` is optional project firmware;
2. decrypted content first: `.3ds`/`.cci`/`.cxi`/`.app`, `.3dsx`/`.elf`;
3. the software renderer first, with the gate on it; the GPU bridge is its
   own phase;
4. every source of nondeterminism pinned, and the host-time leak in the fs
   read delay patched.

## Phases

| Phase | What | State |
|---|---|---|
| 0 | Azahar's libraries build as a miniBox guest; a native twin from the same CMake | done |
| 1 | the driver: boot, frames, the machine's own filesystem, pinned settings, native == sandbox | done |
| 2 | commercial games, the keys slot, save data in and out, touch, motion, lag | done (motion unproven on a game) |
| - | the software-renderer gate | done: waterbox/run-gate.sh, 50 passed / 0 failed with the four test games; 3 passed / 8 skipped with none |
| 3 | the GPU bridge (OpenGL through Chimera's bridge) | done: Mesa llvmpipe in the gate, and a GTX 1060 on Windows (2026-09-28) |
| 4 | CI, the roster, a release | not started (needs the user) |

## How it is built

Azahar's own CMake builds `citra_core`, `video_core` and `audio_core` twice,
with one option set (`waterbox/configure-flags.sh`): the host toolchain for
the native reference (`build/native`), miniBox's musl toolchain for the guest
(`build/guest`). The adapter in `waterbox/` is compiled against the flags
CMake gave the libraries (`extract-tu-flags.py`) and linked into `run-native`
(`native.mk`) and `core.wbx` (`guest.mk`, `build-core.sh`).

    git submodule update --init extern/azahar
    # the submodules Azahar needs (the rest are frontend, network or Vulkan):
    (cd extern/azahar && git submodule update --init --depth 1 \
        externals/boost externals/cryptopp externals/dds-ktx externals/fmt \
        externals/xbyak externals/dynarmic externals/inih/inih externals/nihstro \
        externals/faad2/faad2 externals/library-headers externals/soundtouch \
        externals/teakra externals/zstd externals/enet externals/libressl \
        externals/lodepng/lodepng externals/xxHash dist/compatibility_list)
    (cd extern/azahar/externals/dynarmic && git submodule update --init --depth 1 \
        externals/mcl externals/robin-map externals/zydis externals/zycore)
    waterbox/build-native.sh && make -C waterbox -f native.mk
    waterbox/build-package.sh -o build/package
    waterbox/run-gate.sh

LibreSSL is checked out because Azahar's externals CMake reads its folder,
but it is not built (patch 0003).

## The machine's filesystem

Azahar keeps the console's NAND, SD card, save data and system data as a host
folder tree. Here FileUtil's primitives go to `waterbox/chimera-fs.cpp`
(patch 0005): an in-memory tree in guest memory, so a savestate carries it.
The project's files are served read-only, each through ONE descriptor opened
at Init in a fixed order (the #118 lesson: a descriptor a savestate cannot
carry must never be relied on across a load). Streams are stdio FILEs made
with `fopencookie`, so FileUtil's own code is unchanged.

## Determinism: what was pinned and where

| Source | Fix |
|---|---|
| RTC at power-on (host clock + DST) | FixedTime, `rtc_start` setting |
| mktime in the host's time zone (made native and sandbox an hour apart) | patch 0012: UTC |
| random initial tick count | Fixed, 0 |
| CryptoPP AutoSeededRandomPool / OpenSSL RAND_bytes (console id, MAC, ssl:C, NFC, AM) | patch 0002: one splitmix64 stream in guest memory |
| async I/O on host threads | `deterministic_async_operations`, `async_fs_operations=false` |
| fs read delay minus host time taken | patch 0007 |
| HTTP on a host thread | patch 0003: fails at once on the machine's thread |
| software rasterizer workers | patch 0011: scanlines in order on the machine's thread |
| HOST_TICK | patch 0009: the machine's clock |
| LLE applets from NAND | HLE applets |
| an IPC reply's unwritten bytes (a pushed u8 or bool) came from the host heap | patch 0013: the command buffer is zeroed |
| audio stretching / FIFO | patch 0006: the null sink takes frames as the DSP makes them |

## The gate

`waterbox/run-gate.sh` (`-q` skips the rebuild). The machine legs run on
decrypted dumps in `tests/roms-local`: `darkwitch.cci` (Legend of Dark
Witch), `drancia.cci` (Drancia Saga), `mlss.3ds` (Mario & Luigi Superstar
Saga + Bowser's Minions, the 800-wide top screen) and `cars2.3ds` (Cars 2,
the dump still marked encrypted). Every comparison has a control that can
fail: an idle run for the input legs, the top screen for touch, the default
spelled out for the clock, a scrambled ExeFS for the encryption check, and
MALLOC_PERTURB_ for the host heap (that leg found patch 0013). About 22
minutes on this machine; the sandbox runs about half native speed.

## The GPU bridge (renderer: opengl-hw)

Azahar's OpenGL renderer draws on a real GPU outside the sandbox through
miniBox's bridge (`waterbox/gl-shim.cpp`, the generated
`generated-gl/`, `gl-entry-points.txt`). What it took:

- **No mapped buffers** (patch 0014): the stream buffers keep their bytes in
  the machine's memory and hand them over with `glBufferSubData`.
- **The picture**: the screens go straight into a framebuffer the window binds
  (the libretro path), read back on `SwapBuffers`.
- **The GPU's pictures belong in the console's memory at every frame's end**:
  the driver calls the rasterizer's `FlushAll`, so a savestate taken between
  frames holds everything the GPU drew.
- **A load moves the context**: every load (chimera's host mints a new
  context id), and every state opened in a new process, makes the driver
  throw the renderer away and make it again (`GPU::RecreateRenderer`, a
  shader manager for the running title, and `OpenGLState::ChimeraForget`
  because the state cache's belief is a static the load brought back). The
  caches refill from the console's memory. A run loaded around every frame,
  or into a new host, is byte-identical to one that never stopped.

Open on the GPU side:

- **On a real GPU (GTX 1060, Windows, NVIDIA 581.42, chimera-run --gpu,
  2026-09-28):** all four test games draw correctly through 1800 frames
  (Dark Witch, Drancia Saga, Mario & Luigi, Cars 2). On Dark Witch:
  - 20 state loads in one session: the pictures at 1200 and 1500 are
    byte-identical to a run that never loaded.
  - 30 seeks back to frame 1500 (`--rewind-loop 1500,30`): the last frame
    is byte-identical to a straight run's.
  - A state saved in one process and opened in another: the last frame is
    byte-identical too.
  A straight run's `--final-screenshot` is an undrawn buffer (blank); compare
  against `--screenshot <last frame>` instead.
- **Could a same-session load leak what the frames after the save created?**
  The reasoning: the host keeps one context and mints a new id, and objects
  the dropped renderer made and nobody names any more would stay in the
  driver. NOT OBSERVED on the 1060: over the 30 seeks above (each replaying
  300 frames), the process's GPU dedicated memory stayed between 54 and
  89 MB and ended at 54 MB, and private memory settled at about 640 MB.
  Worth re-measuring over thousands of loads before calling it closed.
- Internal resolution above 1x needs the state to carry the renderer's
  surfaces: see "Internal resolution" below.
- The GPU's pictures reach the console's memory, so under opengl-hw the
  machine itself depends on the driver: a movie replays on the same driver.

## The picture's settings, and the user name (user-decided, 2026-10-09, chimera#223)

Asked for: every graphics option BizHawk's 3DS core has, the screen layout
changeable while a game runs, and the user name. The owner took internal
resolution, the screen layout, the user name, and "the texture filter and
other picture options, each only if the machine's memory is unchanged";
asked how a layout would change in an open project, he chose a normal
setting over a new way to change one live - changing it restarts the
machine and clears the greenzone like any other.

**The layout** is Azahar's own. `Screen Layout` (stacked, single, large,
side-by-side), `Swap Screens`, `Upright Screens` and `Large Screen
Proportion` set Azahar's layout settings; the picture's size is
`GetMinimumSizeFromLayout`, fixed at Init, and the window's
layout comes from `UpdateCurrentFramebufferLayout` as in any Azahar
frontend. The OpenGL renderer draws into it. The software renderer has no
presentation of its own, so the adapter puts each LCD into the rectangle
the layout gives it - the nearest pixel where a screen is not at its own
size, turned a quarter when upright - and the two renderers' pictures were
compared by eye in the stacked, large and upright layouts. The package
declares no virtual size any more: a 3DS's pixels are square and the
picture is its own shape, 720x240 side by side.

**A touch** is still a place in the picture, and Azahar maps it through the
layout. So the layout is the picture only, with one consequence the
declaration states: a movie that touches wants the layout it was made
with. The gate touches the same point of the bottom screen stacked and side
by side - (200, 333) of 400x480 and (560, 93) of 720x240 - and the machine
comes out the same.

**What was measured, and what it decided** (Dark Witch, 900 frames, OpenGL
on llvmpipe; RAM digest against 1x stacked):

| setting | RAM | picture | so |
|---|---|---|---|
| large, large + swap, side-by-side | same | differs | the picture only |
| linear filtering off (large + swap) | same | differs | the picture only |
| internal resolution 2x | DIFFERS | 800x960 | a setting of the machine: see the next section |
| texture filter xBRZ (at 2x) | DIFFERS from 2x | differs | not offered |
| texture sampling "linear" | DIFFERS, at 1x and 2x | - | not offered |
| texture sampling "nearest" | same | same | (nothing to offer alone) |

The texture filter and the forced sampling fail the owner's test and stay
off.

On the GTX 1060 (Cars 2, 1300 frames, the engine's Windows build): FCRAM
is the same file stacked, large and swapped, and large and swapped with
linear filtering off; the small screen is visibly sharper with the filter
off; and in the side-by-side layout the frames drawn after a state load are
the pictures they were (0.00% of pixels, 720x240).

**Not taken:** Azahar's stereoscopic options and 3D intensity - the
intensity is the console's 3D slider, which Azahar writes into the shared
page and the pad state games read (shared_page.cpp, hid.cpp), and the
render options show nothing without it; the background colour and the
custom layout (no colour or rectangle setting to declare them with).

**The user name** is the console's own setting (`cfg`'s user name block,
written before anything runs and saved to the NAND as the settings menu
would): one to ten characters, empty for Azahar's AZAHAR. Cars 2 reads it -
another name changes that game's RAM, the default spelled out does not.

The picture and the readback's rows live in invisible memory now: they are
remade every frame, which is nothing a state should carry.

**What the gate had been letting through.** Adding these legs showed the
savestate legs passing on a sandbox that never started: the runner's memory
layout had fallen behind the package's for an hour, every sandboxed run died
at Init, and "save+load around every frame changes nothing" compared two
equally empty outputs and said PASS. Every comparison of two runs now asks
first that the run reported a frame (`alive`), and compares the frames
reported and not the files (`same`) - the sandbox's host says a line of its
own on the same descriptor when invisible memory is first used.

## Internal resolution (user-decided, 2026-10-10, chimera#223)

`Internal Resolution` (1x to 4x) makes the OpenGL renderer draw at a
multiple of the console's resolution. It was measured on 2026-10-09 and left
out, because a savestate loaded at 2x did not give the same run back. The
reporter asked for it and the owner said to do it now. This section says
what was wrong and how it was fixed.

**It is a setting of the machine, not only of the picture.** At the end of
every frame the core writes what the GPU drew into the console's memory, at
the console's size. Scaled down from a 2x drawing, those bytes are not the
bytes a 1x frame leaves. So the RAM differs between resolutions and a movie
wants the resolution it was made with. The setting's description says so.

**Why a load changed the run.** After every load the core throws the
renderer away and makes it again (see "The GPU bridge"). At 1x the new
renderer fills its surface cache from the console's memory, which holds
every surface exactly. Above 1x the console's memory only holds the
scaled-down copies; the full-size pictures existed only as textures on the
graphics card. The rebuilt renderer scaled the small copies back up, which
are not the pixels that were drawn, and everything drawn on top of them
came out slightly different.

**The fix: the state carries the surface cache** (patch 0015,
`waterbox/azahar-surfaces.cpp`). Above 1x:

- Before the engine takes a state it calls the core's `StateSaving` export.
  The core walks every surface registered in Azahar's rasterizer cache and
  writes it into a block of the core's own memory: its parameters, its
  flags, which address ranges of it are valid, and its pixels read back from
  the card at full size. That block is ordinary memory, so it is part of the
  state.
- After a load, once the renderer has been made again, the core re-creates
  each surface from the block, in the order they were first registered, and
  uploads its pixels.
- All registered surfaces are kept, including ordinary 1x textures, not only
  the upscaled ones. Which surfaces the cache holds decides what it does
  next (for example which existing surface a new framebuffer is laid over),
  so a cache rebuilt with only some of them could behave differently later.
- At 1x nothing of this runs. The block is not even mapped.

To keep states small and saving fast, a surface keeps its place in the
block for as long as it stays registered, a surface whose modification
counter has not moved is not read again, and what is read is compared with
the block one memory page at a time so only pages that changed are written
(a state is stored as the pages that changed). The save path allocates
nothing: taking a state must not change the machine, the heap included.

Limits: the block is 1 GiB of address space (pages are only committed when
written) and holds up to 8192 surfaces. If something does not fit, the core
says so once on stderr and those surfaces are rebuilt from the console's
memory after a load, as before the fix.

**Measured on Mesa llvmpipe** (the test script's software GL; "straight" is
a run that never loads, "told" loads a state around every frame, "untold"
does the same with `StateSaving` not called, which is the old behaviour):

| game, resolution, frames | told vs straight | untold vs straight | state size, untold -> told |
|---|---|---|---|
| Dark Witch, 2x, 300 | same | differs from frame 90 | 128 -> 135 MB |
| Dark Witch, 4x, 900 | same | differs from frame 90 | 137 -> 176 MB |
| Cars 2, 2x, 2400 | same | differs from frame 1650 | 163 -> 182 MB |
| Mario & Luigi, 3x, 900 | same | same (nothing in these frames shows it) | 227 -> 259 MB |
| Drancia Saga, 4x, 600 | same | same (nothing in these frames shows it) | 137 -> 185 MB |

In all five, a state saved in one process and loaded in a new one also gave
the straight run, and the native build and the sandbox agreed. "Same" means
the RAM, picture and audio digests of every 30th frame are equal.

**Measured on a real card** (GTX 1060, Windows, NVIDIA 581.42, the engine's
headless runner; Cars 2 with a movie that presses A and touches the screen
until it is in a race, which it is from about frame 3000). At 2x and at 4x,
each of these ended with the same FCRAM, the same VRAM and the same picture
at frame 3650 as a run of 3700 frames that never took a state:

- the same run with a state saved at frame 3400;
- that state opened in a new process and run to frame 3700;
- the same again with a state saved and loaded around every frame;
- at 2x, twenty rewinds from frame 3700 back to 3400 through the greenzone.

With the engine told not to call `StateSaving`
(`CHIMERA_NO_STATE_SAVING=1`), the load-every-frame run ended with another
VRAM and another picture at both resolutions. At 2x the twelve frames drawn
after a greenzone load were each the picture they had been (0.00% of pixels
differ); untold, the first differed in 59% of its pixels and the picture
was not right until the sixth.

What it costs on that card: 3700 frames take 62 s at 2x and 114 s at 4x
with no states taken. With a state stored on every single frame at 2x,
3620 frames took 145 s told and 102 s untold, so about 12 ms for each state
taken in a 3D scene. A saved state was 202 MB at 2x and 238 MB at 4x.

Not tested: 3x on a real card (only on llvmpipe), other drivers, and
TAStudio itself (the runs above are the headless runner).

## Lag

A game reads its controls straight out of HID's (and IR:RST's) shared
memory; no service call says so. Patch 0008 routes those pages through the
memory system's watchpoint path (the pointer in the page table is cleared,
so dynarmic takes the slow path for them) and marks the frame on a read.

## Open

- Phase 3: the GPU bridge. The evaluation's notes: persistent buffer mapping
  must go (PCSX2 patch 0015's shape), and a state load must rebuild the
  rasterizer cache including every cached descriptor (the #153 lesson).
- Speed: the software renderer draws on one thread; Dark Witch's intro runs
  at about 15 frames a second native and 8 in the sandbox.
- `.cia` installs, seeded eShop titles and anything needing `aes_keys.txt`
  are declared but not tested (no keys here).
- Motion input is wired and declared but no test game uses it.
- The HLE software keyboard answers with its default text; not seen in a
  game yet.
- Home and Power buttons are not on the panel (they need the HOME menu).
