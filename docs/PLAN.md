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
- Internal resolution above 1x is not offered, and since chimera#223 the
  reason is measured: see "The picture's settings" below.
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
| internal resolution 2x | DIFFERS | 800x960 | not offered: see below |
| texture filter xBRZ (at 2x) | DIFFERS from 2x | differs | not offered |
| texture sampling "linear" | DIFFERS, at 1x and 2x | - | not offered |
| texture sampling "nearest" | same | same | (nothing to offer alone) |

Internal resolution changes memory because of how this core holds the GPU's
pictures: every frame ends by writing them into the console's memory at the
console's size, and what comes down from a 2x drawing is not what a 1x
drawing leaves. That alone would make it a setting of the machine, declared
as one. What rules it out is the other thing the gate said at 2x: native and
sandbox agreed, a state reopened in a new host agreed, and **a load around
every frame did not** - the renderer is made again from the console's
memory after a load, that memory holds the pictures at the console's size,
and the frames drawn on from there are not the frames a run that never
loaded draws. At 1x the round trip is exact, which is the whole of how this
core's GPU states work. A tool that rewinds cannot offer a resolution a
rewind changes the run at, so the drawing path takes a scale (`Machine::
scale`, `kMaxScale`) and nothing sets it. What it would take is what the
other GPU cores got in chimera#190: a state that holds the larger pictures.
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
