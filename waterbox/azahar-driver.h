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
};

/// Boot. `rom` is the game's path in the machine's filesystem (see
/// chimera-fs-host.h). Returns false with Error() set.
bool Init(const Machine& m, const std::string& rom);
const char* Error();
void SetLogLevel(int level);  // Common::Log::Level; default Critical

/// One frame: runs the machine to its next VBlank.
void Frame();
bool InputWasRead();

// The panel (see waterbox.config): buttons and axes, in wire order.
constexpr int kButtons = 15;  // A B X Y Up Down Left Right L R Start Select ZL ZR Touch
constexpr int kAxes = 12;     // Circle Pad X/Y, C-Stick X/Y, Touch X/Y, Accel X/Y/Z, Gyro X/Y/Z
bool ButtonActive(int i);
bool AxisActive(int i);
void SetButton(int i, bool on);
void SetAxis(int i, int32_t value);

const uint32_t* Video(int* w, int* h);  // BGRA, the two screens stacked: 400x480
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
