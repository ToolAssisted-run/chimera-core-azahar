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
| 3 | the GPU bridge (OpenGL through Chimera's bridge) | NOT STARTED |
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
