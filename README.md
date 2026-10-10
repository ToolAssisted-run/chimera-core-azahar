# chimera-core-azahar

[Azahar](https://github.com/azahar-emu/azahar)'s Nintendo 3DS as a
[Chimera](https://github.com/ToolAssisted-run/chimera) core, running in
miniBox's sandbox.

| System | Machine | Panel |
|---|---|---|
| Nintendo 3DS / New Nintendo 3DS | `3DS` | A B X Y, D-pad, L R, Start Select, ZL ZR (New 3DS), Circle Pad, C-Stick (New 3DS), touch screen, motion |

The picture is the two screens stacked, 400x480, unless the project's Screen
Layout says otherwise (one screen, one large and one small, side by side,
swapped, upright). The touch screen is the
**Touch** button plus **Touch X/Y**, a point on the whole picture in
0..65535 (as every absolute position in Chimera is): a point on the bottom
screen touches it, anywhere else touches nothing. Lag frames are counted: a
frame in which the game never read its controls.

## What a project needs

- **A decrypted game**: a cartridge dump (`.3ds`/`.cci`), an executable
  content (`.cxi`/`.app`) or homebrew (`.3dsx`/`.elf`). Azahar does not
  decrypt; an encrypted dump is a load error that says so. An old dump whose
  header still says "encrypted" after it was decrypted is recognised and runs.
- Nothing else for a decrypted game. **The package carries no key, no system
  file and no game.** A project that needs the console's keys (`.cia`
  content, amiibo) brings its own `aes_keys.txt` as firmware, and
  `seeddb.bin` for seeded eShop titles.

## Settings

All of them are part of the machine: a movie needs the same values.

- **Model**: New 3DS (default) or original 3DS.
- **Region**: the game's own (default) or a fixed one.
- **Clock at Power-On**: seconds since 1970, default 2000-01-01. The clock
  then runs with the machine, never with the host.
- **CPU**: dynarmic's recompiler (default) or the interpreter.
- **CPU Clock (%)**.
- **Renderer**: `software` (default: inside the sandbox, the same on every
  machine) or `opengl-hw`, Azahar's OpenGL renderer on the real GPU through
  the bridge - many times faster, but the GPU's pictures reach the console's
  memory, so a movie recorded with it replays only on the same driver.
- **Internal Resolution**: 1x (default) to 4x, with `opengl-hw` only. It
  changes what the game reads back from the GPU, so a movie wants the value
  it was made with. Above 1x savestates are larger.
- **User Name**: the console's own, which games read.
- **Motion Controls**: whether the accelerometer and gyroscope take input.

The picture only, not the machine: **Screen Layout** (stacked, single, large,
side-by-side), **Swap Screens**, **Upright Screens**, **Large Screen
Proportion** and **Linear Filtering**. A touch is a place in the picture, so
a movie that touches wants the layout it was made with.

## Save data

Export Save Data writes the SD card's files (`sdmc/Nintendo 3DS/...`: the
game's save archive and extra data); the project's Save data slot takes that
.zip back, and the game starts from it.

## Using it in Chimera

Chimera includes no cores and downloads none. Download the `.chimeraCore`
file from this repository's
[Releases](https://github.com/ToolAssisted-run/chimera-core-azahar/releases)
page, or build it, and put it in the `Cores` folder beside `Chimera.exe`.
File > Core Manager lists what is in that folder. The same file works on
Linux and on Windows.

## Building

`waterbox/build-package.sh -r <chimera checkout>` builds
`azahar.chimeraCore` into that checkout's `build/Cores`.
[docs/BUILDING.md](docs/BUILDING.md) has every step, and
[AGENTS.md](AGENTS.md) is the guide for an AI coding agent. See
`docs/PLAN.md` for the design, and `waterbox/run-gate.sh` for the gate. The
machine legs need decrypted games in `tests/roms-local` (never committed).

## Licence

This repository's own files are MIT (see `LICENSE`); the patches in
`patches/` are Azahar's terms. Azahar is GPL-2.0-or-later, and so is the
built core; the licences of everything it links travel inside the package
(`waterbox/package-licenses.json`).
