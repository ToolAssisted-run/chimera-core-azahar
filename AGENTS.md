# AGENTS.md - Azahar core for Chimera

This repository builds Azahar (https://github.com/azahar-emu/azahar), a
Nintendo 3DS emulator, as a core for Chimera
(https://github.com/ToolAssisted-run/chimera), a frontend for tool-assisted
speedruns. It produces one file, `azahar.chimeraCore`: Azahar's core
libraries compiled as a guest for Chimera's sandbox (miniBox), an adapter,
and the declarations Chimera reads. Upstream is a pinned submodule; every
change to it is a numbered patch.

## Layout

- `extern/azahar` - upstream Azahar, a pinned git submodule.
- `patches/` - the numbered series applied to `extern/azahar`.
- `waterbox/azahar-driver.cpp`, `wbx-entry.cpp` - the machine (boot, one
  frame per call, panel, picture, sound) and the exports Chimera calls.
- `waterbox/chimera-fs.cpp`, `zip-read.cpp`, `guest-syscalls.cpp` - the
  machine's filesystem in guest memory, the save data zip, libc overrides.
- `waterbox/gl-shim.cpp`, `gl-host.c`, `gl-entry-points.txt`, `glad/` - the
  GPU bridge: guest side, host side, the entry points wrapped.
- `waterbox/run-native.c`, `run-wbx.c`, `gate-harness.h` - the two harnesses
  the gate compares, and the options they share.
- `waterbox/*.sh`, `guest.mk`, `native.mk`, `guest-toolchain.cmake` - the
  builds and the gate. `configure-flags.sh` holds Azahar's CMake options.
- `waterbox/gen-config.py` - writes `waterbox.config`, `file_slots.json` and
  `default_keybinds.json`. Those three are generated.
- `docs/PLAN.md` - the design. `docs/BUILDING.md` - the build in detail.
- `.github/workflows/chimera.yml` - CI: gate, contract tests, release.
- `build/`, `waterbox/bin`, `waterbox/obj-*`, `waterbox/generated-gl`,
  `tests/roms-local/` - build output and local games. Ignored by git.

## Set up the build environment

Ubuntu, as CI. `<chimera>` is a Chimera checkout; `<core>` is this one.

```
sudo apt-get update
sudo apt-get install -y --no-install-recommends meson ninja-build build-essential cmake pkg-config python3 mono-complete xvfb libgl1-mesa-dev libegl-dev libx11-dev libxext-dev libasound2-dev

git clone --recursive https://github.com/ToolAssisted-run/chimera.git <chimera>
export CHIMERA_ROOT=<chimera>
export MINIBOX_DIR=<chimera>/extern/chimera-common-minibox

cd <core>
git submodule update --init --depth 1 extern/azahar
(cd extern/azahar && git submodule update --init --depth 1 \
  externals/boost externals/cryptopp externals/dds-ktx externals/fmt \
  externals/xbyak externals/dynarmic externals/inih/inih externals/nihstro \
  externals/faad2/faad2 externals/library-headers externals/soundtouch \
  externals/teakra externals/zstd externals/enet externals/libressl \
  externals/lodepng/lodepng externals/xxHash dist/compatibility_list)
(cd extern/azahar/externals/dynarmic && git submodule update --init --depth 1 \
  externals/mcl externals/robin-map externals/zydis externals/zycore)

mb="$MINIBOX_DIR"
[ -f "$mb/build/meson-linux/build.ninja" ] || meson setup "$mb/build/meson-linux" "$mb"
meson compile -C "$mb/build/meson-linux"
[ -f "$mb/build/meson-cpp/build.ninja" ] || meson setup "$mb/build/meson-cpp" "$mb" -Dguest_cpp=true
meson compile -C "$mb/build/meson-cpp"
```

The miniBox guest toolchain build downloads the GCC source that matches the
host compiler. Chimera's contract tests also need the .NET SDK 8.0 and
Chimera's own build: see `docs/BUILDING.md`.

## Build

```
cd <core>
waterbox/build-package.sh -r "$CHIMERA_ROOT"
```

On a fresh tree it applies the patches and builds the guest libraries
(`build/guest`). It always links `waterbox/bin/core.wbx`, regenerates the
declarations and writes `$CHIMERA_ROOT/build/Cores/azahar.chimeraCore`. With
`-o <dir>` it writes `<dir>/azahar.chimeraCore` and installs nothing. After
changing a patch, run `waterbox/build-guest.sh` first: `build-package.sh`
runs it only when `build/guest/Makefile` does not exist.

The native reference, for debugging and for the gate:

```
waterbox/build-native.sh
make -C waterbox -f native.mk MB="$MINIBOX_DIR"
```

## Install the core into Chimera

Chimera ships no cores and downloads nothing. A core is a file in its cores
folder: `<chimera>/build/Cores/` in a source checkout (where
`build-package.sh -r <chimera>` writes), or the `Cores` folder beside
`Chimera.exe` in a release bundle, or the folder chosen in File > Core
Manager > Change folder... File > Core Manager lists the folder; Refresh
List rescans it. The same package works on Linux and on Windows. A package
built by hand stamps `<commit>+local` (`-dirty` with changes in the tree,
which the applied patches count as) and is for testing. Only CI's packages
are published.

## Test before you commit

```
waterbox/run-gate.sh
```

It builds the native reference and the package (`build/package/`), then
prints `PASS`, `FAIL` or `SKIP` for every leg. `-q` skips the rebuild. Zero
failed is the bar. Logs are in `build/gate/`.

- With no game it proves the builds, that `core.wbx` is not stale, that the
  declarations are the generator's and that the core carries no key blob.
  That is all CI can run.
- The machine legs need decrypted dumps in `tests/roms-local/` (or the
  folder `AZAHAR_ROMS` names): `darkwitch.cci`, `drancia.cci`, `mlss.3ds`,
  `cars2.3ds`. A change to the machine must be run with them. If you do not
  have them, say so in your report: a run with every machine leg skipped
  does not show the machine still works.
- CI then runs Chimera's contract tests against the package
  (`docs/BUILDING.md`, Run the gates).

## Rules of this repository

- `extern/azahar` is a submodule. Never commit inside it. A change to Azahar
  is a numbered patch in `patches/` (`NNNN-chimera-<what-it-does>.patch`, a
  plain `git diff`), applied by `waterbox/apply-patches.sh`. The series is
  all or nothing: a half-patched tree is an error. A diff of a file that two
  patches touch holds both changes; read "The shape of a core repository" in
  Chimera's `docs/porting-a-core.md` before regenerating one.
- Determinism is the product. The guest must not read host time, host
  randomness, the host's heap or anything else that differs between runs,
  and a savestate must round-trip. The gate checks it. A change that breaks
  it is a bug.
- Run the gate before committing. A new leg needs a negative control: show
  that it fails when the thing it checks is broken (Chimera's
  `docs/gates.md`).
- Never commit game files, keys or firmware. Never add network access. The
  build keeps `-DENABLE_BUILTIN_KEYBLOB=OFF`: the package carries no key.
- Do not hand-edit the three declarations. Edit `waterbox/gen-config.py` and
  run it; its panel must be the one `wbx-entry.cpp` binds.
- Change Azahar's CMake options in `waterbox/configure-flags.sh` only: both
  builds source it, and they must differ only in toolchain.
- The scripts that are run stay executable (git mode 100755);
  `configure-flags.sh` is sourced and is 100644. Prose is plain ASCII.
- Commit messages: a type prefix with an optional scope (`feat(gpu):`,
  `fix(gate):`, `docs(plan):`, `test(gate):`, `ci:`, `build:`), then a
  sentence in lower case that says what is now true. The body says what was
  wrong, what was measured and what the gate printed.
- Do not edit `.github/workflows` unless the task is the workflow.

## Where to read more

- `docs/BUILDING.md` - every build step, option and error message.
- `docs/PLAN.md` - phases, the determinism table, the GPU bridge, what is open.
- `README.md` - what a project needs, the settings, the licence.
- In a Chimera checkout: `docs/porting-a-core.md`, `docs/gates.md`,
  `docs/core-manager.md`.
