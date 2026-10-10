// The adapter's surface: wbx-entry.cpp (the guest ABI) and the gate's native
// harness are its two callers.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ChimeraAzahar
{
// The machine a project pins. Every field is part of the machine: a movie
// needs the same values to play back.
struct Machine
{
  bool new3ds = true;
  int region = -1;           // -1 auto (from the game), else Azahar's 0 JPN .. 6 TWN
  uint64_t rtc_start = 946684800;  // the RTC at power-on, seconds since 1970 (2000-01-01)
  bool cpu_jit = true;
  int cpu_clock = 100;       // percent
  bool motion = false;       // the accelerometer and gyroscope take input
  bool opengl = false;       // the OpenGL renderer through the GPU bridge (else software)
  // The console's user name, as its settings menu would hold it: games read
  // it (a save's owner, a greeting). Empty: Azahar's own ("AZAHAR"). Ten
  // characters at most, as the console allows.
  std::string username;

  // How many times the console's own resolution the OpenGL renderer draws
  // at, 1 to kMaxScale. THE MACHINE, unlike the picture's settings below: at
  // 2x each frame's pictures come back into the console's memory brought down
  // from the larger drawing - other bytes than a 1x frame leaves there - so a
  // movie made at one scale is not a movie at another. Above 1 a state has to
  // hold the larger pictures themselves (azahar-surfaces.cpp), or a load
  // would not give the run back.
  int scale = 1;

  // ---- the picture: none of these is the machine (the gate holds each to
  // that: the RAM the same at every value, under both renderers) ----
  // How the two screens are put into one picture (Azahar's layouts):
  // 0 stacked, the top one above the bottom; 1 one screen only; 2 one large
  // and one small beside it; 3 side by side.
  int layout = 0;
  bool swap_screens = false;     // the bottom screen takes the top one's place
  bool upright = false;          // the console turned on its side
  int large_proportion = 4;      // layout 2: how many times larger the large screen is
  bool linear_filter = true;     // the screens are scaled into the picture with a linear filter (OpenGL)
};

// The largest picture any of the above makes, a side: two screens side by
// side at the highest scale.
constexpr int kMaxScale = 4;
constexpr int kMaxSide = (400 + 320) * kMaxScale;

/// Where the picture and its scratch row buffer live: `pixels` 32-bit pixels
/// each. Set before Init by a host that has somewhere better than the heap -
/// the sandbox keeps them out of every savestate (they are remade every
/// frame). Null: the heap.
extern uint32_t* (*VideoMemory)(size_t pixels);

/// Where the OpenGL renderer's surfaces are kept for a savestate, above the
/// console's resolution (azahar-surfaces.cpp): `block` is ordinary memory, in
/// every state, and may be address space nothing has touched yet; `scratch` is
/// memory no state carries, 4 MiB or more. Set before Init, and at the same
/// point of every run: whether memory is mapped is part of a state. Without
/// it a state loaded above 1x draws on from the console's memory.
void SurfaceMemory(void* block, size_t blockBytes, void* scratch, size_t scratchBytes);

/// Boot. `rom` is the game's path in the machine's filesystem (see
/// chimera-fs-host.h). Returns false with Error() set.
bool Init(const Machine& m, const std::string& rom);
const char* Error();
void SetLogLevel(int level);  // Common::Log::Level; default Critical

/// One frame: runs the machine to its next VBlank.
void Frame();
/// The engine loaded a state into the machine (the StateLoaded export).
void StateLoaded();
/// The engine is about to take a state of the machine (the StateSaving export).
void StateSaving();
/// Which renderer is drawing: "software" or "opengl".
const char* Renderer();
bool InputWasRead();

// The panel (see waterbox.config): buttons and axes, in wire order.
constexpr int kButtons = 15;  // A B X Y Up Down Left Right L R Start Select ZL ZR Touch
constexpr int kAxes = 12;     // Circle Pad X/Y, C-Stick X/Y, Touch X/Y, Accel X/Y/Z, Gyro X/Y/Z
bool ButtonActive(int i);
bool AxisActive(int i);
void SetButton(int i, bool on);
void SetAxis(int i, int32_t value);

const uint32_t* Video(int* w, int* h);  // BGRA, the screens as the layout puts them; 400x480 by default
const int16_t* Audio(int* frames);      // stereo pairs at 32728 Hz, this frame's

struct Domain
{
  const char* name;
  uint8_t* ptr;
  int64_t size;
};
std::vector<Domain> Domains();

// Save data: what the game keeps on its SD card and in its save archive, as
// files named by their path under the machine's user folder.
struct SaveFile
{
  std::string name;
  std::vector<uint8_t> bytes;
};
std::vector<SaveFile> ExportSaveData();
}  // namespace ChimeraAzahar
