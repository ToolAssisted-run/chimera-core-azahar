# Building the Azahar core

This repository builds Azahar, a Nintendo 3DS emulator, as a sandboxed guest
for Chimera. The result is one file, `azahar.chimeraCore`, which Chimera
loads. The steps below are the ones `.github/workflows/chimera.yml` runs on a
fresh clone on a public Ubuntu runner; where the workflow uses a GitHub
Action, the manual equivalent is given.

Placeholders used below:

- `<core>` - this repository's checkout.
- `<chimera>` - a checkout of https://github.com/ToolAssisted-run/chimera.
- `<miniBox>` - `<chimera>/extern/chimera-common-minibox`, the sandbox host
  and the guest toolchain, a git submodule of Chimera.

## Requirements

CI builds on `ubuntu-latest`. Cores are built on Linux; the package that
comes out runs on Linux and on Windows.

The packages the workflow installs:

```
sudo apt-get update
sudo apt-get install -y --no-install-recommends meson ninja-build build-essential cmake pkg-config python3 mono-complete xvfb libgl1-mesa-dev libegl-dev libx11-dev libxext-dev libasound2-dev
```

Toolchains:

- **C and C++**: the `gcc` and `g++` that `build-essential` installs. The
  workflow pins no compiler version. Use one `gcc` for miniBox and for the
  core (see Troubleshooting).
- **.NET SDK 8.0**: the workflow uses `actions/setup-dotnet@v4` with
  `dotnet-version: '8.0'`. By hand, Chimera's README gives
  `curl -sSL https://dot.net/v1/dotnet-install.sh | bash -s -- --channel 8.0`
  and says a distribution's own SDK lacks targets the frontend needs. Make
  sure `dotnet` is on `PATH` afterwards. It is needed only to build Chimera's
  solution and run its contract tests, not to build the package.
- **Mono** (`mono-complete`): for Chimera's managed side, which targets .NET
  Framework 4.8 and runs on Mono on Linux.
- **git**, for the clone and the submodules.

What the build fetches or generates by itself:

- miniBox's C++ guest toolchain (`-Dguest_cpp=true`) downloads the GCC source
  that matches the host compiler (about 84 MB) with `curl` and builds
  libstdc++ for the guest from it. That step needs `curl` and the network.
- This repository's scripts download nothing. Everything else comes from the
  submodules.
- `waterbox/build-core.sh` generates the GPU bridge's wrappers into
  `waterbox/generated-gl/` with miniBox's `source/gl/gen-gl-bridge.py`.
- Build output goes to `build/`, `waterbox/bin/`, `waterbox/obj-guest/` and
  `waterbox/obj-native/`. All of it is ignored by git.

## Get the sources

This repository, and only the submodules the core builds with. The rest of
Azahar's submodules are frontend, network or Vulkan, and some are large.

```
git clone https://github.com/ToolAssisted-run/chimera-core-azahar.git <core>
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
```

LibreSSL is checked out because Azahar's externals CMake reads its folder; it
is not built (patch 0003).

Chimera, with its submodules. The workflow uses `actions/checkout@v6` with
`repository: ToolAssisted-run/chimera`, `ref: main` and
`submodules: recursive`:

```
git clone --recursive https://github.com/ToolAssisted-run/chimera.git <chimera>
```

Where the scripts look for Chimera and miniBox:

- `waterbox/build-package.sh` takes `-r <chimera>`. Without it, it tries
  `<core>/../chimera`, then `$HOME/chimera`.
- `waterbox/run-gate.sh` takes `-r <chimera>` or `CHIMERA_ROOT`, and falls
  back to `$HOME/chimera`.
- miniBox is `-m <miniBox>` or `MINIBOX_DIR`. The fallback is
  `<chimera>/extern/chimera-common-minibox`, or
  `$HOME/chimera/extern/chimera-common-minibox` in the scripts and makefiles
  that are not told where Chimera is.

The simplest way to be right everywhere is to export both, as the workflow's
gate step does:

```
export CHIMERA_ROOT=<chimera>
export MINIBOX_DIR=<chimera>/extern/chimera-common-minibox
```

## Build miniBox

The host library and the C++ guest toolchain, in two build directories. These
are the workflow's commands:

```
mb="$MINIBOX_DIR"
[ -f "$mb/build/meson-linux/build.ninja" ] || meson setup "$mb/build/meson-linux" "$mb"
meson compile -C "$mb/build/meson-linux"
[ -f "$mb/build/meson-cpp/build.ninja" ] || meson setup "$mb/build/meson-cpp" "$mb" -Dguest_cpp=true
meson compile -C "$mb/build/meson-cpp"
```

- `build/meson-linux` holds the host library (`source/host/libminiboxhost.so`)
  that `run-wbx`, the gate's sandbox runner, links.
- `build/meson-cpp` holds the guest sysroot (`guest-sysroot/`: musl, the
  `musl-gcc.specs` file and the guest's `libstdc++.a`).

CI caches these two directories with `actions/cache@v4`. By hand they simply
stay where they are.

## Build the core

All commands run from `<core>`, with `MINIBOX_DIR` exported.

**Patches.** Everything that changes Azahar is a numbered patch in
`patches/`, applied to the `extern/azahar` working tree by
`waterbox/apply-patches.sh`. Both build scripts run it first. It judges the
series as a whole:

- a pristine submodule gets every patch, in order (`applied: <name>` each);
- a tree that already carries the whole series is left alone
  (`already applied: all N patches`);
- anything in between is an error that names the files that differ.

Before touching the tree it tries the series on a scratch copy of the
submodule's HEAD, so a series that does not apply stops there.

**The guest.** Azahar's own CMake under the miniBox musl toolchain
(`waterbox/guest-toolchain.cmake`), with the options in
`waterbox/configure-flags.sh`. Only `citra_core`, `video_core` and
`audio_core` are built, into `build/guest`:

```
waterbox/build-guest.sh
```

`JOBS=<n>` sets the number of make jobs (default: `nproc`). Extra arguments
go to `make`.

**core.wbx.** `waterbox/build-core.sh` compiles the adapter (`guest.mk`, into
`waterbox/obj-guest/`), links it with the guest archives into
`waterbox/bin/core.wbx`, checks it with miniBox's `check-wbx.sh`, and builds
`waterbox/bin/run-wbx`, the host runner the gate uses:

```
waterbox/build-core.sh -m "$MINIBOX_DIR"
```

Its options are `-m <miniBox dir>`, `-o <output dir>` and `-j N`.

**The native reference.** The same driver, exports and Azahar libraries built
for the host with the same option set, into `build/native`, and a harness,
`waterbox/obj-native/run-native`, that calls the exports directly. The gate
compares it with `core.wbx` in the sandbox, frame by frame. It is also where
a debugger works.

```
waterbox/build-native.sh
make -C waterbox -f native.mk MB="$MINIBOX_DIR"
```

The package does not need the native reference. The gate builds it.

## Build the package

```
waterbox/build-package.sh -r <chimera>
```

Options:

- `-r <chimera root>` - the Chimera checkout.
- `-m <miniBox dir>` - miniBox, when it is not `<chimera>/extern/chimera-common-minibox`.
- `-o <dir>` - write `<dir>/azahar.chimeraCore` and install nothing.

What it does:

1. Runs `waterbox/build-guest.sh` if `build/guest/Makefile` does not exist,
   then always `waterbox/build-core.sh`.
2. Runs `waterbox/gen-config.py`, which rewrites `waterbox/waterbox.config`,
   `waterbox/file_slots.json` and `waterbox/default_keybinds.json`.
3. Stages `core.wbx`, those three declarations, the licences of everything
   linked (from `waterbox/package-licenses.json`) and `build.json` (what
   built the package: commit, compiler, miniBox commit, Azahar pin).
4. Stamps the version, zips the result twice with fixed dates and
   permissions, and fails if the two archives differ. It prints
   `package sha1 <hash>` and `packaged -> <path>`.

Where the file lands:

- Without `-o`: `<chimera>/build/Cores/azahar.chimeraCore`. The folder is
  created if needed, and `<chimera>/build/CoreCache/azahar-*` is removed so
  Chimera does not run a stale extracted copy.
- With `-o <dir>`: `<dir>/azahar.chimeraCore` only.

The version stamp:

- A package's version is the commit it was built from. CI sets
  `CORE_VERSION` to the commit, and that is what a published package carries.
- Without `CORE_VERSION` the script stamps `<commit>+local`, and
  `<commit>-dirty+local` when `git diff --quiet HEAD` reports a change. The
  applied patch series makes the `extern/azahar` working tree differ from its
  commit, and git counts that, so a package built by hand normally reads
  `-dirty+local`.
- The commit's date in UTC is stamped beside it as `versionDate`.

A package built by hand is for testing. Chimera's publishing refuses a
version that carries `+local` or `-dirty`.

## Install it into Chimera

Chimera ships no cores and downloads nothing: it has no network code. A core
is a file somebody puts in its cores folder.

- **A Chimera source checkout.** The cores folder is `<chimera>/build/Cores/`.
  `waterbox/build-package.sh -r <chimera>` writes the package straight there.
  A package built with `-o` is copied in by hand, as the workflow does:

  ```
  mkdir -p <chimera>/build/Cores
  cp build/package/azahar.chimeraCore <chimera>/build/Cores/
  ```

- **A Chimera release bundle.** Put the file in the `Cores` folder beside
  `Chimera.exe`, or in another folder chosen with File > Core Manager >
  Change folder...

File > Core Manager lists what is in the folder. Refresh List rescans it, so
a package copied in while Chimera runs is found without a restart.

The same package file works on Linux and on Windows: the guest inside it is
run by miniBox on either.

Published packages are on this repository's Releases page. CI publishes a
rolling `dev` release on every green push to main, and a dated
`nightly-YYYY-MM-DD` release from the scheduled run (04:00 UTC, only when
main moved since the last one). The asset is named
`azahar-<version>.chimeraCore`.

## Run the gates

CI runs two things: this repository's gate, then Chimera's contract tests
against the package the gate built. CI allows the whole job 180 minutes.

### The core's gate

```
waterbox/run-gate.sh
```

Options: `-r <chimera root>`, `-m <minibox dir>`, and `-q` to skip the
rebuild and use what `build/` and `waterbox/bin` hold. It reads
`CHIMERA_ROOT`, `MINIBOX_DIR` and `AZAHAR_ROMS`. Each leg prints `PASS`,
`FAIL` or `SKIP` with a line of evidence; the last line counts them. The
script exits 1 if a leg failed or if nothing passed. Its work directory is
`build/gate/`, emptied at the start of every run.

The legs that need no game, which are all that run on a public runner:

- the native reference and its harness build (`build/gate/native.log`,
  `build/gate/native-mk.log`);
- the package builds, into `build/package/azahar.chimeraCore`
  (`build/gate/package.log`);
- `core.wbx` is newer than the adapter sources it is built from;
- the three declarations are exactly what `waterbox/gen-config.py` writes;
- the core carries no key blob: the option is off in `configure-flags.sh`
  and the key symbol is absent from `core.wbx`, with a control that the
  symbol listing is not empty.

The machine legs need decrypted dumps, which cannot be distributed. Put them,
or links to them, in `tests/roms-local/` (ignored by git), or point
`AZAHAR_ROMS` at a folder that holds them. The gate looks for these names:
`darkwitch.cci`, `drancia.cci`, `mlss.3ds`, `cars2.3ds`. A game that is not
there is skipped, and the `SKIP` line names the missing file. With all of
them absent, none of the legs below runs:

- per game: the panel is the declared one; the machine is alive; two native
  runs are the same; nothing from the host's heap reaches the machine
  (`MALLOC_PERTURB_`); native == sandbox over 600 exercised frames, compared
  as a stream of per-frame digests; the input reaches the machine (an idle
  run differs); lag frames are counted; a state saved and loaded around every
  frame changes nothing; a state reopens in a new host.
- `drancia.cci`: a button press and a touch reach the machine (a touch on the
  top screen reaches nothing); save data goes out and back in; a file that is
  not a save zip is refused; the clock at power-on and the model are part of
  the machine.
- `cars2.3ds`: an encrypted dump, made from it on the spot, is refused.
- the engine: the package through `<chimera>/build/meson-linux/chimera-run`
  with a movie. It needs `drancia.cci`, `chimera-run` and the package.
- the GPU bridge (`renderer: opengl-hw`) on `darkwitch.cci` and
  `drancia.cci`: the OpenGL renderer through a headless EGL context (Mesa's
  llvmpipe in CI), native == sandbox, the fall back to the software renderer
  when no bridge is handed over, and states through a renderer rebuild.

Every comparison has a negative control beside it. Run the full gate, with
the games, before pushing: CI cannot.

### Chimera's contract tests

They open the package through Chimera's engine, so Chimera's native libraries
and its solution must be built first. The workflow's commands, from
`<chimera>`:

```
meson setup build/meson-linux --prefix "$PWD/build" --libdir dll
meson compile -C build/meson-linux
meson install -C build/meson-linux
dotnet build source/gui/Chimera.sln -c Release /nodeReuse:false -p:UseSharedCompilation=false
```

That build also produces `build/meson-linux/chimera-run`, which the gate's
engine legs use. Then, with the package in `<chimera>/build/Cores`:

```
CHIMERA_CORES_DIR=<chimera>/build/Cores dotnet test source/gui/Chimera.Tests.Client.Common/Chimera.Tests.Client.Common.csproj \
  -c Release --nologo \
  --filter "FullyQualifiedName~InstalledCorePackagesTests|FullyQualifiedName~MnemonicUniquenessTests"
```

They prove the package is readable, is built for a guest ABI this frontend
runs, makes a working core factory, binds only buttons its controller
declares, and stamps a version. They need no game.

## Files the core needs at run time

Nothing below is in this repository or in the package. The user provides it.

- **Game** (required, one file): a decrypted dump. A cartridge (`.3ds`,
  `.cci`), an executable content (`.cxi`, `.app`) or homebrew (`.3dsx`,
  `.elf`). Azahar does not decrypt; an encrypted dump is a load error that
  says so.
- **Save data** (optional): a `.zip` as Emulator > Export Save Data... wrote
  it. It goes onto the machine's SD card before the game starts.
- **Firmware** (optional, off by default):
  - `aes_keys.txt`, the console's AES keys in Azahar's text format, dumped
    from the user's own console. Required only when the AES Keys setting is
    on. A decrypted game needs none.
  - `seeddb.bin`, the title seeds some eShop titles are encrypted with.
    Required only when the Seed Database setting is on, which is used only
    with AES Keys.

The package is built with `-DENABLE_BUILTIN_KEYBLOB=OFF`: it carries no key,
no system file and no game. The gate checks that.

The Renderer setting chooses `software` (the default, inside the sandbox) or
`opengl-hw`, Azahar's OpenGL renderer on the host's GPU through Chimera's GPU
bridge. On a machine that offers no GL context the core draws with the
software renderer and says so.

## Troubleshooting

- **`extern/azahar is not checked out`** (`apply-patches.sh`). The submodule
  is missing. Run the submodule commands in Get the sources.
- **`extern/azahar is partly patched`**. Some touched files are neither
  pristine nor what the whole series leaves. The script prints the way back:
  `git -C extern/azahar reset --hard && git -C extern/azahar clean -fd && waterbox/apply-patches.sh`.
  That discards edits made in the tree; turn them into a patch first.
- **`the series does not apply to the submodule's HEAD at <patch>`**. The
  submodule was moved without rebasing the patches.
- **`miniBox guest sysroot not found`** or **`miniBox C++ guest toolchain
  missing`**. `<miniBox>/build/meson-cpp` was not built with
  `-Dguest_cpp=true`, or the script looked in the wrong place. Set
  `MINIBOX_DIR`.
- **A script looks in `$HOME/chimera`.** `build-guest.sh`, `build-core.sh`,
  `guest.mk` and `native.mk` fall back to
  `$HOME/chimera/extern/chimera-common-minibox` when they are not told where
  miniBox is. Export `MINIBOX_DIR`, pass `-m`, or pass `MB=` to the
  makefiles.
- **One gcc for miniBox and the core.** The guest's C++ headers are looked up
  by the version `gcc -dumpfullversion` prints
  (`waterbox/guest-toolchain.cmake`, `waterbox/guest.mk`), and miniBox builds
  the guest libstdc++ from the GCC source that matches the host compiler.
- **A changed patch is not in the package.** `build-package.sh` runs
  `build-guest.sh` only when `build/guest/Makefile` does not exist, and the
  gate builds the package through it. After changing a patch or moving the
  submodule, run `waterbox/build-guest.sh` yourself. The native reference is
  rebuilt by the gate every time, so a stale guest shows as a native ==
  sandbox failure.
- **`build/guest missing - run build-guest.sh first`** (`build-core.sh`).
  Run it, or use `build-package.sh`.
- **`chimera checkout not found; pass -r <path>`** (`build-package.sh`).
- **`the declarations are gen-config.py's` fails.** One of the three
  declaration files was edited by hand. Edit `waterbox/gen-config.py` and run
  it.
- **`NOTHING RAN`** at the end of the gate: no leg passed. Read the logs in
  `build/gate/`. CI uploads that folder as the `gate-work` artifact when the
  job fails.
