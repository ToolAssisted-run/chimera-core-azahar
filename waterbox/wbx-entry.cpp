// The chimera guest ABI: azahar-driver wrapped into the miniBox core exports
// (the same surface as the other chimera cores). Compiled for the guest, and
// natively for the gate's reference run - the native build finds its files in
// the working directory, which run-native fills the way the frontend fills
// the sandbox.
// SPDX-License-Identifier: MIT

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <emulibc.h>
#include <initializer_list>

#include <waterbox_settings.h>
#include <waterbox_slots.h>

#include "azahar-driver.h"
#include "chimera-fs-host.h"
#include "gl-shim.h"
#include "zip-read.h"

namespace
{
char g_loadError[1024];

bool Exists(const char* name)
{
  FILE* f = fopen(name, "rb");
  if (!f)
    return false;
  fclose(f);
  return true;
}

const char* const kButtonNames[ChimeraAzahar::kButtons] = {
    "A", "B", "X", "Y", "Up", "Down", "Left", "Right", "L", "R", "Start", "Select", "ZL", "ZR",
    "Touch"};

int RegionOf(const char* s)
{
  static const char* const names[] = {"jpn", "usa", "eur", "aus", "chn", "kor", "twn"};
  for (int i = 0; i < 7; i++)
    if (!strcmp(s, names[i]))
      return i;
  return -1;
}

// Which of `names` a setting's value is, or -1.
int OneOf(const char* s, std::initializer_list<const char*> names)
{
  int i = 0;
  for (const char* name : names)
  {
    if (!strcmp(s, name))
      return i;
    i++;
  }
  return -1;
}

// Init's answer to a setting it cannot honour.
int Refuse(const char* format, const char* value)
{
  snprintf(g_loadError, sizeof g_loadError, format, value);
  return 0;
}

// The picture and its readback rows are remade every frame from the machine:
// they live where no savestate looks.
uint32_t* PictureMemory(size_t pixels)
{
  return static_cast<uint32_t*>(alloc_invisible(pixels * sizeof(uint32_t)));
}

std::vector<ChimeraAzahar::SaveFile> g_saves;
}  // namespace

extern "C" {

ECL_EXPORT const char* GetLoadError(void)
{
  return g_loadError;
}

ECL_EXPORT int Init(void)
{
  g_loadError[0] = '\0';

  // An optional log level, as a mounted file (a guest has no environment):
  // 0 trace .. 5 critical. Diagnostics only; never part of a project.
  if (FILE* f = fopen("loglevel", "rb"))
  {
    int level = 5;
    if (fscanf(f, "%d", &level) == 1)
      ChimeraAzahar::SetLogLevel(level);
    fclose(f);
  }

  // The game: the project mounts it under its own name and names it in the
  // "game" slot; a file opened directly arrives with its name in rom.name.
  char rom[512] = "";
  if (!wbx_slot_first("game", rom, sizeof rom))
  {
    if (FILE* f = fopen("rom.name", "rb"))
    {
      size_t n = fread(rom, 1, sizeof rom - 1, f);
      while (n && (rom[n - 1] == '\n' || rom[n - 1] == '\r'))
        n--;
      rom[n] = '\0';
      fclose(f);
    }
  }
  const char* base = strrchr(rom, '/');
  base = base ? base + 1 : rom;
  // A game opened on its own (no project) is mounted as "game", its real name
  // in rom.name; the machine still sees the real name, whose extension says
  // what kind of file it is.
  const char* host = Exists(base) ? base : "game";
  if (!base[0] || !Exists(host))
  {
    snprintf(g_loadError, sizeof g_loadError, "no game: the project names none, or '%s' is not there", base);
    return 0;
  }
  // The project's files, each opened once, in this order, every run (see
  // chimera-fs.cpp for why the order matters).
  const std::string romPath = std::string("/game/") + base;
  if (!ChimeraFSHost::MountFile(romPath, host))
  {
    snprintf(g_loadError, sizeof g_loadError, "cannot open the game '%s'", base);
    return 0;
  }
  // Keys and seeds, if the project carries them: a decrypted cartridge needs
  // neither. Upstream would compile Nintendo's keys into the core; this one
  // is built without them (ENABLE_BUILTIN_KEYBLOB=OFF).
  for (const char* sys : {"aes_keys.txt", "seeddb.bin"})
    if (Exists(sys) && !ChimeraFSHost::MountFile(std::string("/user/sysdata/") + sys, sys))
    {
      snprintf(g_loadError, sizeof g_loadError, "cannot open %s", sys);
      return 0;
    }

  // What the game already saved: the .zip Export Save Data writes, whose
  // entries are the SD card's files (sdmc/Nintendo 3DS/...). They are the
  // machine's own files from here on - written into its filesystem before
  // Init, so they are part of the sealed machine and not of every savestate.
  char sd[512] = "";
  if (wbx_slot_first("savedata", sd, sizeof sd))
  {
    const char* sdBase = strrchr(sd, '/');
    sdBase = sdBase ? sdBase + 1 : sd;
    std::vector<ZipFile> files;
    std::string error;
    if (!ZipReadAll(sdBase, files, error))
    {
      snprintf(g_loadError, sizeof g_loadError, "the save data '%s' is not a save data zip: %s", sdBase, error.c_str());
      return 0;
    }
    if (files.empty())
    {
      snprintf(g_loadError, sizeof g_loadError, "the save data '%s' holds no files", sdBase);
      return 0;
    }
    for (const auto& f : files)
    {
      if (f.name.compare(0, 5, "sdmc/") != 0 || f.name.find("..") != std::string::npos)
      {
        snprintf(g_loadError, sizeof g_loadError,
                 "the save data holds '%s', which is not the SD card's: every entry is sdmc/..., as "
                 "Export Save Data writes them", f.name.c_str());
        return 0;
      }
      ChimeraFSHost::WriteFile("/user/" + f.name, f.bytes.data(), f.bytes.size());
    }
  }

  ChimeraAzahar::VideoMemory = PictureMemory;
  ChimeraAzahar::Machine m;
  char val[64];
  if (wbx_setting_str("model", val, sizeof val) > 0)
    m.new3ds = strcmp(val, "old3ds") != 0;
  if (wbx_setting_str("region", val, sizeof val) > 0)
    m.region = RegionOf(val);
  m.rtc_start = static_cast<uint64_t>(wbx_setting_long("rtc_start", 946684800L));
  if (wbx_setting_str("cpu", val, sizeof val) > 0)
    m.cpu_jit = strcmp(val, "interpreter") != 0;
  m.cpu_clock = static_cast<int>(wbx_setting_long("cpu_clock", 100));
  m.motion = wbx_setting_bool("motion", 0) != 0;
  if (wbx_setting_str("renderer", val, sizeof val) > 0)
    m.opengl = strcmp(val, "opengl-hw") == 0;
  {
    char name[64];
    if (wbx_setting_str("username", name, sizeof name) > 0)
      m.username = name;
  }
  // The picture. A value this build does not know is an error rather than a
  // default: a project that asks for a layout gets that layout or is told.
  if (wbx_setting_str("layout", val, sizeof val) > 0)
  {
    m.layout = OneOf(val, {"stacked", "single", "large", "side-by-side"});
    if (m.layout < 0)
      return Refuse("no such screen layout: %s", val);
  }
  m.swap_screens = wbx_setting_bool("swap_screens", 0) != 0;
  m.upright = wbx_setting_bool("upright", 0) != 0;
  m.large_proportion = static_cast<int>(wbx_setting_long("large_screen_proportion", 4));
  m.linear_filter = wbx_setting_bool("linear_filter", 1) != 0;

  if (!ChimeraAzahar::Init(m, romPath))
  {
    snprintf(g_loadError, sizeof g_loadError, "%s", ChimeraAzahar::Error());
    return 0;
  }
  return 1;
}

ECL_EXPORT int GetButtonCount(void)
{
  return ChimeraAzahar::kButtons;
}

ECL_EXPORT const char* GetButtonName(int32_t index)
{
  return index >= 0 && index < ChimeraAzahar::kButtons ? kButtonNames[index] : "";
}

ECL_EXPORT int IsButtonActive(int32_t index)
{
  return ChimeraAzahar::ButtonActive(index) ? 1 : 0;
}

ECL_EXPORT void SetButton(int32_t index, int32_t state)
{
  ChimeraAzahar::SetButton(index, state != 0);
}

ECL_EXPORT int GetAxisCount(void)
{
  return ChimeraAzahar::kAxes;
}

ECL_EXPORT int IsAxisActive(int32_t index)
{
  return ChimeraAzahar::AxisActive(index) ? 1 : 0;
}

ECL_EXPORT void SetAxis(int32_t index, int32_t value)
{
  ChimeraAzahar::SetAxis(index, value);
}

ECL_EXPORT void FrameAdvance(uint64_t)
{
  ChimeraAzahar::Frame();
}

// The GPU bridge (see gl-shim.cpp): the host hands its callback over BEFORE
// Init, where the renderer is chosen.
ECL_EXPORT void SetGpuBridge(uint64_t addr)
{
  chimera_azahar_install_gpu_bridge(addr);
}

// The engine calls this after every load of the machine, with the machine
// stopped (see CheckGLContext in azahar-driver.cpp).
ECL_EXPORT void StateLoaded(void)
{
  ChimeraAzahar::StateLoaded();
}

ECL_EXPORT int InputWasRead(void)
{
  return ChimeraAzahar::InputWasRead() ? 1 : 0;
}

// Every frame is drawn: the software renderer's picture is the machine's own
// framebuffer read out, so skipping it saves nothing a seek would notice.
ECL_EXPORT void SetRenderingEnabled(int)
{
}

static int g_vw, g_vh, g_an;

ECL_EXPORT uint32_t* GetVideoBgra(void)
{
  return const_cast<uint32_t*>(ChimeraAzahar::Video(&g_vw, &g_vh));
}

ECL_EXPORT int GetVideoWidth(void)
{
  ChimeraAzahar::Video(&g_vw, &g_vh);
  return g_vw;
}

ECL_EXPORT int GetVideoHeight(void)
{
  ChimeraAzahar::Video(&g_vw, &g_vh);
  return g_vh;
}

ECL_EXPORT int16_t* GetAudio(void)
{
  return const_cast<int16_t*>(ChimeraAzahar::Audio(&g_an));
}

ECL_EXPORT int GetAudioSampleCount(void)
{
  ChimeraAzahar::Audio(&g_an);
  return g_an;
}

// The LCDs refresh every 4481136 cycles of the 268111856 Hz ARM11: 59.83 Hz.
ECL_EXPORT int GetVsyncNumerator(void)
{
  return 268111856;
}

ECL_EXPORT int GetVsyncDenominator(void)
{
  return 4481136;
}

ECL_EXPORT int GetMemoryDomainCount(void)
{
  return static_cast<int>(ChimeraAzahar::Domains().size());
}

ECL_EXPORT const char* GetMemoryDomainName(int i)
{
  const auto d = ChimeraAzahar::Domains();
  return i >= 0 && i < static_cast<int>(d.size()) ? d[i].name : "";
}

ECL_EXPORT uint8_t* GetMemoryDomainPtr(int i)
{
  const auto d = ChimeraAzahar::Domains();
  return i >= 0 && i < static_cast<int>(d.size()) ? d[i].ptr : nullptr;
}

ECL_EXPORT int64_t GetMemoryDomainSize(int i)
{
  const auto d = ChimeraAzahar::Domains();
  return i >= 0 && i < static_cast<int>(d.size()) ? d[i].size : 0;
}

ECL_EXPORT int GetMemoryDomainWritable(int i)
{
  return i >= 0 && i < GetMemoryDomainCount() ? 1 : 0;
}

// Save data: taken fresh when the count is asked for, handed out by index.
ECL_EXPORT int32_t GetSaveDataFileCount(void)
{
  g_saves = ChimeraAzahar::ExportSaveData();
  return static_cast<int32_t>(g_saves.size());
}

ECL_EXPORT const char* GetSaveDataFileName(int32_t i)
{
  return i >= 0 && i < static_cast<int32_t>(g_saves.size()) ? g_saves[i].name.c_str() : "";
}

ECL_EXPORT int64_t GetSaveDataFileSize(int32_t i)
{
  return i >= 0 && i < static_cast<int32_t>(g_saves.size()) ? static_cast<int64_t>(g_saves[i].bytes.size()) : 0;
}

ECL_EXPORT const uint8_t* GetSaveDataFileBuffer(int32_t i)
{
  return i >= 0 && i < static_cast<int32_t>(g_saves.size()) ? g_saves[i].bytes.data() : nullptr;
}

}  // extern "C"
