// Azahar as a Chimera machine: boot, one frame per call, the panel, the
// picture, the sound. The emulator is upstream's core library untouched but for
// the patch series (see patches/); this file is the whole frontend.
// SPDX-License-Identifier: MIT

#include "azahar-driver.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <tuple>

#include "chimera-fs-host.h"
#include "gl-shim.h"

#include "audio_core/audio_types.h"
#include "common/file_util.h"
#include "common/logging/backend.h"
#include "common/settings.h"
#include "common/vector_math.h"
#include "core/3ds.h"
#include "core/core.h"
#include "common/string_util.h"
#include "core/frontend/applets/default_applets.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/framebuffer_layout.h"
#include "core/frontend/image_interface.h"
#include "core/frontend/input.h"
#include "core/hle/service/cfg/cfg.h"
#include "core/hle/kernel/config_mem.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/shared_page.h"
#include "core/hle/service/service.h"
#include "core/loader/loader.h"
#include "core/memory.h"
#include <glad/glad.h>
#include "video_core/gpu.h"
#include "video_core/renderer_base.h"
#include "video_core/renderer_opengl/gl_state.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_software/renderer_software.h"

namespace ChimeraAzahar
{
namespace
{
std::string g_error;
Machine g_machine;
bool g_booted = false;
bool g_gl = false;          // the OpenGL renderer, through the GPU bridge
bool g_stateLoaded = false; // StateLoaded since the last frame (see CheckGLContext)
bool g_frameDone = false;
bool g_inputRead = false;

bool g_buttons[kButtons];
int32_t g_axes[kAxes];

// The picture: its size is the layout's own smallest (Azahar's
// GetMinimumSizeFromLayout - 400x480 for the two screens stacked) times the
// scale, fixed at Init.
int g_w = Core::kScreenTopWidth;
int g_h = Core::kScreenTopHeight + Core::kScreenBottomHeight;
uint32_t* g_video = nullptr;  // g_w * g_h
uint32_t* g_rows = nullptr;   // the same again: glReadPixels' bottom-up rows
std::vector<int16_t> g_audio;

// Wire order of the panel's axes.
enum Axis
{
  kCircleX,
  kCircleY,
  kCStickX,
  kCStickY,
  kTouchX,
  kTouchY,
  kAccelX,
  kAccelY,
  kAccelZ,
  kGyroX,
  kGyroY,
  kGyroZ,
};
constexpr int kTouchButton = 14;

void ResetInputs()
{
  std::fill(std::begin(g_buttons), std::end(g_buttons), false);
  std::fill(std::begin(g_axes), std::end(g_axes), 0);
  g_axes[kTouchX] = 32768;
  g_axes[kTouchY] = 32768;
  g_axes[kAccelZ] = -1000;  // at rest: one g along -Z
}

// ---- the panel, as Azahar's input devices -----------------------------------
// HID polls these on its own schedule inside a frame; they answer what the
// frontend set before the frame began, so every poll of a frame agrees.
class Button final : public Input::ButtonDevice
{
public:
  explicit Button(int i) : index(i) {}
  bool GetStatus() const override { return index >= 0 && index < kButtons && g_buttons[index]; }

private:
  int index;
};

class ButtonFactory final : public Input::Factory<Input::ButtonDevice>
{
public:
  std::unique_ptr<Input::ButtonDevice> Create(const Common::ParamPackage& params) override
  {
    return std::make_unique<Button>(params.Get("button", -1));
  }
};

class Stick final : public Input::AnalogDevice
{
public:
  explicit Stick(int first) : x(first), y(first + 1) {}
  std::tuple<float, float> GetStatus() const override
  {
    auto f = [](int32_t v) { return std::clamp(static_cast<float>(v) / 127.0f, -1.0f, 1.0f); };
    return {f(g_axes[x]), f(g_axes[y])};
  }

private:
  int x, y;
};

class StickFactory final : public Input::Factory<Input::AnalogDevice>
{
public:
  std::unique_ptr<Input::AnalogDevice> Create(const Common::ParamPackage& params) override
  {
    return std::make_unique<Stick>(params.Get("axis", 0) == 1 ? kCStickX : kCircleX);
  }
};

// Accelerometer in thousandths of a g, gyroscope in tenths of a degree per
// second - the units the panel declares.
class Motion final : public Input::MotionDevice
{
public:
  std::tuple<Common::Vec3<float>, Common::Vec3<float>> GetStatus() const override
  {
    if (!g_machine.motion)
      return {{0.0f, 0.0f, -1.0f}, {0.0f, 0.0f, 0.0f}};
    const Common::Vec3<float> accel{g_axes[kAccelX] / 1000.0f, g_axes[kAccelY] / 1000.0f,
                                    g_axes[kAccelZ] / 1000.0f};
    const Common::Vec3<float> gyro{g_axes[kGyroX] / 10.0f, g_axes[kGyroY] / 10.0f,
                                   g_axes[kGyroZ] / 10.0f};
    return {accel, gyro};
  }
};

class MotionFactory final : public Input::Factory<Input::MotionDevice>
{
public:
  std::unique_ptr<Input::MotionDevice> Create(const Common::ParamPackage&) override
  {
    return std::make_unique<Motion>();
  }
};

// ---- the window: where the picture and the touch panel meet ------------------
class Window final : public Frontend::EmuWindow
{
public:
  Window() { UpdateCurrentFramebufferLayout(g_w, g_h); }

  // Called at every VBlank (RendererBase::EndFrame): the frame is over.
  void PollEvents() override
  {
    if (!g_gl)
      Compose();
    g_frameDone = true;
  }

  // ---- the OpenGL renderer (the GPU bridge) ----
  // The renderer draws both screens, laid out, into whatever framebuffer this
  // binds (patch 0014 takes the libretro frontend's path); SwapBuffers reads
  // the picture back into the machine's own buffer.
  void SetupFramebuffer() override
  {
    if (!g_gl)
      return;
    if (fbo == 0)
    {
      glGenRenderbuffers(1, &color);
      glBindRenderbuffer(GL_RENDERBUFFER, color);
      glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, g_w, g_h);
      glGenFramebuffers(1, &fbo);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
      glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
  }

  bool NeedsClearing() const override
  {
    return true;
  }

  void SwapBuffers() override
  {
    if (!g_gl || fbo == 0)
      return;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, g_w, g_h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, g_rows);
    // GL's rows run bottom-up
    for (int y = 0; y < g_h; y++)
    {
      const uint32_t* from = g_rows + static_cast<size_t>(g_h - 1 - y) * g_w;
      uint32_t* to = g_video + static_cast<size_t>(y) * g_w;
      for (int x = 0; x < g_w; x++)
        to[x] = from[x] | 0xFF000000u;
    }
    // back to what the renderer's state cache believes is bound
    const auto cur = OpenGL::OpenGLState::GetCurState();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, cur.draw.read_framebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, cur.draw.draw_framebuffer);
  }

  // The framebuffer was made before the load: let it go (a name that means
  // nothing in this context is ignored by glDelete*) and make it again on the
  // next frame.
  void ForgetGL()
  {
    if (fbo)
      glDeleteFramebuffers(1, &fbo);
    if (color)
      glDeleteRenderbuffers(1, &color);
    fbo = 0;
    color = 0;
  }

  // The touch panel, from the Touch button and the two Touch axes. The axes
  // are a fraction of the WHOLE stacked picture (0..65535 across 400 and down
  // 480), so a pointer on the picture needs no knowledge of where the bottom
  // screen is; a touch outside the bottom screen touches nothing.
  void ApplyTouch()
  {
    const bool down = g_buttons[kTouchButton];
    const unsigned x = static_cast<unsigned>(std::clamp(g_axes[kTouchX], 0, 65535)) * g_w / 65536;
    const unsigned y = static_cast<unsigned>(std::clamp(g_axes[kTouchY], 0, 65535)) * g_h / 65536;
    if (down && touching)
      TouchMoved(x, y);
    else if (down)
      touching = TouchPressed(x, y);
    else if (touching)
    {
      TouchReleased();
      touching = false;
    }
  }

private:
  bool touching = false;
  GLuint fbo = 0, color = 0;

  // One LCD's picture into its place in the frame - the rectangle Azahar's
  // layout gives that screen, which is the LCD's own size when the two are
  // stacked or side by side and anything else when one is large and one small.
  // ScreenInfo holds the LCD's portrait framebuffer; addressed as below it
  // reads out landscape (see citra_libretro's blit), `lcdWidth` x 240. A top
  // screen in the 800 pixel "wide" mode some 2D games use is twice as wide as
  // that: each pair of its pixels is averaged into one. A rectangle of another
  // size takes the nearest of the LCD's pixels; `upright` is the console
  // turned on its side, the rectangle 240 wide for a screen 240 high.
  static void Blit(const SwRenderer::ScreenInfo& info, const Common::Rectangle<u32>& to,
                   u32 lcdWidth, bool upright)
  {
    const u32 w = info.height, h = std::min<u32>(info.width, Core::kScreenTopHeight);
    if (w == 0 || h == 0 || info.pixels.size() < static_cast<size_t>(w) * info.width * 4)
      return;
    const u32 step = w > lcdWidth ? w / lcdWidth : 1;
    const u32 cols = std::min(lcdWidth, w / step);
    const u32 rw = std::min<u32>(to.GetWidth(), g_w - std::min<u32>(to.left, g_w));
    const u32 rh = std::min<u32>(to.GetHeight(), g_h - std::min<u32>(to.top, g_h));
    const u32 fw = to.GetWidth(), fh = to.GetHeight();
    if (cols == 0 || fw == 0 || fh == 0)
      return;
    for (u32 y = 0; y < rh; y++)
    {
      uint32_t* dst = g_video + static_cast<size_t>(to.top + y) * g_w + to.left;
      for (u32 x = 0; x < rw; x++)
      {
        // the LCD's pixel under this one
        u32 sx, sy;
        if (!upright)
        {
          sx = x * cols / fw;
          sy = y * h / fh;
        }
        else
        {
          sx = (fh - 1 - y) * cols / fh;
          sy = x * h / fw;
        }
        const u8* src = info.pixels.data() + (static_cast<size_t>(sy) * w + sx * step) * 4;
        u32 r = 0, g = 0, b = 0;
        for (u32 k = 0; k < step; k++, src += 4)
        {
          r += src[0];
          g += src[1];
          b += src[2];
        }
        dst[x] = 0xFF000000u | ((r / step) << 16) | ((g / step) << 8) | (b / step);
      }
    }
  }

  void Compose()
  {
    std::fill(g_video, g_video + static_cast<size_t>(g_w) * g_h, 0xFF000000u);
    auto& r = static_cast<SwRenderer::RendererSoftware&>(Core::System::GetInstance().GPU().Renderer());
    const Layout::FramebufferLayout& layout = GetFramebufferLayout();
    const bool upright = !layout.is_rotated;
    if (layout.top_screen_enabled)
      Blit(r.Screen(VideoCore::ScreenId::TopLeft), layout.top_screen, Core::kScreenTopWidth, upright);
    if (layout.bottom_screen_enabled)
      Blit(r.Screen(VideoCore::ScreenId::Bottom), layout.bottom_screen, Core::kScreenBottomWidth,
           upright);
  }
};

std::unique_ptr<Window> g_window;

void ApplyMachine(const Machine& m)
{
  auto& v = Settings::values;
  // The machine the project pins.
  v.is_new_3ds = m.new3ds;
  v.region_value = m.region;
  v.use_cpu_jit = m.cpu_jit;
  v.use_fastinterp = true;
  v.cpu_clock_percentage = m.cpu_clock;
  v.init_clock = Settings::InitClock::FixedTime;
  v.init_time = m.rtc_start;
  v.init_time_offset = 0;
  // Determinism, none of it a choice: a fixed tick count at power-on (upstream
  // draws it from the host), every service's "asynchronous" work run on the
  // machine's thread, applets as the HLE ones (a NAND dump is not required,
  // and an LLE applet would be one more thing a project has to carry).
  v.init_ticks_type = Settings::InitTicks::Fixed;
  v.init_ticks_override = 0;
  v.deterministic_async_operations = true;
  v.async_fs_operations = false;
  v.lle_applets = false;
  v.plugin_loader_enabled = false;
  v.use_virtual_sd = true;
  v.use_custom_storage = false;
  for (const auto& service_module : Service::service_module_map)
    v.lle_modules[service_module.name] = false;
  // Output: the software renderer (the GPU bridge is a later phase), no
  // frame limiter, the sound handed over as the DSP makes it.
  v.graphics_api = g_gl ? Settings::GraphicsAPI::OpenGL : Settings::GraphicsAPI::Software;
  v.use_gles = false;
  v.use_hw_shader = true;
  v.shaders_accurate_mul = true;
  v.use_shader_jit = true;
  // Azahar's texture filters and its forced texture sampling are not offered:
  // each changes what a frame leaves in the console's memory (measured, see
  // docs/PLAN.md), and nothing here may do that unasked.
  v.texture_filter = Settings::TextureFilter::NoFilter;
  v.texture_sampling = Settings::TextureSampling::GameControlled;
  v.filter_mode = m.linear_filter;
  v.frame_limit = 0;
  v.use_vsync = false;
  v.output_type = AudioCore::SinkType::Null;
  v.input_type = AudioCore::InputType::Null;
  v.enable_audio_stretching = false;
  v.audio_emulation = Settings::AudioEmulation::HLE;
  // The picture's shape: Azahar's own layouts, at the size each one asks for.
  static const Settings::LayoutOption layouts[] = {
      Settings::LayoutOption::Default, Settings::LayoutOption::SingleScreen,
      Settings::LayoutOption::LargeScreen, Settings::LayoutOption::SideScreen};
  v.layout_option = layouts[std::clamp(m.layout, 0, 3)];
  v.swap_screen = m.swap_screens;
  v.upright_screen = m.upright;
  v.large_screen_proportion = static_cast<float>(std::clamp(m.large_proportion, 1, 16));
  v.small_screen_position = Settings::SmallScreenPosition::BottomRight;
  v.resolution_factor = g_gl ? static_cast<u32>(std::clamp(m.scale, 1, kMaxScale)) : 1;
  {
    const auto size = Layout::GetMinimumSizeFromLayout(v.layout_option.GetValue(), m.upright);
    g_w = static_cast<int>(size.first * v.resolution_factor.GetValue());
    g_h = static_cast<int>(size.second * v.resolution_factor.GetValue());
  }
  v.use_disk_shader_cache = false;
  v.async_shader_compilation = false;
  v.dump_textures = false;
  v.custom_textures = false;
  v.preload_textures = false;
  v.render_3d = Settings::StereoRenderOption::Off;
  // The panel.
  auto& p = v.current_input_profile;
  for (int i = 0; i < Settings::NativeButton::NumButtons; i++)
    p.buttons[i] = "engine:none";
  const std::pair<int, int> map[] = {
      {Settings::NativeButton::A, 0},     {Settings::NativeButton::B, 1},
      {Settings::NativeButton::X, 2},     {Settings::NativeButton::Y, 3},
      {Settings::NativeButton::Up, 4},    {Settings::NativeButton::Down, 5},
      {Settings::NativeButton::Left, 6},  {Settings::NativeButton::Right, 7},
      {Settings::NativeButton::L, 8},     {Settings::NativeButton::R, 9},
      {Settings::NativeButton::Start, 10}, {Settings::NativeButton::Select, 11},
      {Settings::NativeButton::ZL, 12},   {Settings::NativeButton::ZR, 13},
  };
  for (const auto& [native, wire] : map)
    p.buttons[native] = "engine:chimera,button:" + std::to_string(wire);
  p.analogs[Settings::NativeAnalog::CirclePad] = "engine:chimera,axis:0";
  p.analogs[Settings::NativeAnalog::CStick] = "engine:chimera,axis:1";
  p.motion_device = "engine:chimera";
  p.touch_device = "engine:emu_window";
  p.controller_touch_device = "";
  p.udp_input_address = "";
}
}  // namespace

uint32_t* (*VideoMemory)(size_t pixels) = nullptr;

const char* Error()
{
  return g_error.c_str();
}

void SetLogLevel(int level)
{
  Common::Log::SetDirectLevel(static_cast<Common::Log::Level>(level));
}

bool Init(const Machine& m, const std::string& rom)
{
  g_machine = m;
  ResetInputs();
  FileUtil::SetUserPath("/user/");
  // The OpenGL renderer draws through the GPU bridge a host handed over before
  // Init; without one the machine keeps the software renderer, and says so.
  g_gl = false;
  if (m.opengl)
  {
    if (!ChimeraGL::Present())
      fprintf(stderr, "[azahar] renderer opengl asked for, but no GPU bridge: software renderer\n");
    else if (!ChimeraGL::Load())
      fprintf(stderr, "[azahar] the GPU bridge could not load OpenGL: software renderer\n");
    else
      g_gl = true;
  }
  ApplyMachine(m);
  {
    // the picture and the readback's rows: twice the picture
    const size_t pixels = static_cast<size_t>(g_w) * g_h;
    if (!g_video)
    {
      // the most pixels a layout holds: the two screens stacked, at the
      // highest scale (side by side is fewer)
      const size_t most = static_cast<size_t>(Core::kScreenTopWidth) *
                          (Core::kScreenTopHeight + Core::kScreenBottomHeight) * kMaxScale * kMaxScale;
      g_video = VideoMemory ? VideoMemory(2 * most) : new uint32_t[2 * most];
      if (!g_video)
      {
        g_error = "no memory for the picture";
        return false;
      }
      g_rows = g_video + most;
    }
    std::fill(g_video, g_video + pixels, 0xFF000000u);
  }

  Input::RegisterFactory<Input::ButtonDevice>("chimera", std::make_shared<ButtonFactory>());
  Input::RegisterFactory<Input::AnalogDevice>("chimera", std::make_shared<StickFactory>());
  Input::RegisterFactory<Input::MotionDevice>("chimera", std::make_shared<MotionFactory>());

  auto& system = Core::System::GetInstance();
  Frontend::RegisterDefaultApplets(system);
  system.RegisterImageInterface(std::make_shared<Frontend::ImageInterface>());

  g_window = std::make_unique<Window>();
  // The game's header first, the way the libretro frontend does it: System::
  // Load reports an encrypted game as "cannot determine the system mode",
  // which tells a person nothing. The loader is handed on, not opened twice.
  {
    auto loader = Loader::GetLoader(rom);
    if (!loader)
    {
      g_error = "no loader for the game file (" + rom + ")";
      return false;
    }
    const auto [mode, status] = loader->LoadKernelMemoryMode();
    (void)mode;
    if (status == Loader::ResultStatus::ErrorEncrypted)
    {
      g_error = "the game is encrypted: Azahar runs decrypted dumps only (.3ds/.cci/.cxi "
                "decrypted, or .3dsx homebrew)";
      return false;
    }
    if (status == Loader::ResultStatus::ErrorInvalidFormat)
    {
      g_error = "the game file is not a format Azahar reads (.3ds .cci .cxi .app .3dsx .elf)";
      return false;
    }
    if (status == Loader::ResultStatus::ErrorGbaTitle)
    {
      g_error = "a GBA Virtual Console title: Azahar does not run those";
      return false;
    }
    system.RegisterAppLoaderEarly(loader);
  }
  const auto result = system.Load(*g_window, rom);
  switch (result)
  {
  case Core::System::ResultStatus::Success:
    break;
  case Core::System::ResultStatus::ErrorLoader_ErrorEncrypted:
    g_error = "the game is encrypted: Azahar runs decrypted dumps only (.3ds/.cci/.cxi "
              "decrypted, or .3dsx homebrew)";
    return false;
  case Core::System::ResultStatus::ErrorLoader_ErrorInvalidFormat:
    g_error = "the game file is not a format Azahar reads (.3ds .cci .cxi .app .3dsx .elf)";
    return false;
  case Core::System::ResultStatus::ErrorLoader_ErrorGbaTitle:
    g_error = "a GBA Virtual Console title: Azahar does not run those";
    return false;
  case Core::System::ResultStatus::ErrorGetLoader:
    g_error = "no loader for the game file (" + rom + ")";
    return false;
  case Core::System::ResultStatus::ErrorSystemFiles:
    g_error = "a 3DS system archive is missing: " + system.GetStatusDetails();
    return false;
  default:
    g_error = "Azahar could not boot the game (status " + std::to_string(static_cast<int>(result)) +
              "): " + system.GetStatusDetails();
    return false;
  }
  // The console's user name, into its settings before anything has run:
  // what its own settings menu would have written (cfg's user name block).
  if (!m.username.empty())
  {
    const std::u16string name = Common::UTF8ToUTF16(m.username);
    if (name.empty() || name.size() > 10)
    {
      g_error = "the user name is not one the console can hold: one to ten characters";
      return false;
    }
    if (auto cfg = Service::CFG::GetModule(system))
    {
      cfg->SetUsername(name);
      cfg->UpdateConfigNANDSavegame();
    }
  }
  u64 program_id = 0;
  system.GetAppLoader().ReadProgramId(program_id);
  system.GPU().ApplyPerProgramSettings(program_id);
  system.RegisterCoreLoopThreadId();
  g_booted = true;
  return true;
}

// Before the machine steps: has the GL context under the renderer's objects
// moved? Every object the OpenGL renderer holds is a NAME in the machine's
// memory; a savestate brings back names of a context that is gone (another
// process) or of objects the frames after the save have since changed (this
// one, where the host mints a new id on every load). Between frames nothing is
// mid-draw, so the whole renderer is thrown away and made again; its caches
// fill back up from the console's memory, which holds everything the GPU drew
// because every frame ends with a flush (below). The remembered id lives in the
// machine's memory like the names, and a stored 0 cannot vouch for anything once
// a state has been loaded (chimera issue 126, the Dolphin note).
uint64_t g_glContext = 0;

void CheckGLContext()
{
  const bool afterLoad = g_stateLoaded;
  g_stateLoaded = false;
  const uint64_t live = ChimeraGL::ContextId();
  if (live == 0)
    return;
  if (live != g_glContext && (g_glContext != 0 || afterLoad))
  {
    // what the driver has bound is unknown: every Apply from here makes every call
    OpenGL::OpenGLState::ChimeraForget();
    g_window->ForgetGL();
    auto& system = Core::System::GetInstance();
    system.GPU().RecreateRenderer(*g_window, nullptr);
    // A fresh rasterizer has no shader manager until a title switch or a disk
    // cache load makes one; the running title will not switch again, so one is
    // made here (the disk cache itself is off: nothing is read)
    std::atomic_bool stop{false};
    system.GPU().Renderer().Rasterizer()->LoadDefaultDiskResources(stop, nullptr);
  }
  g_glContext = live;
}

void Frame()
{
  if (!g_booted)
    return;
  auto& system = Core::System::GetInstance();
  if (g_gl)
    CheckGLContext();
  g_audio.clear();
  g_inputRead = false;
  g_frameDone = false;
  g_window->ApplyTouch();
  // A VBlank is 4481136 ARM11 cycles away; RunLoop runs a slice at a time.
  for (int guard = 0; !g_frameDone && guard < 1000000; guard++)
  {
    const auto r = system.RunLoop();
    if (r != Core::System::ResultStatus::Success)
    {
      g_error = "the machine stopped: " + system.GetStatusDetails();
      break;
    }
  }
  // Everything the GPU drew goes back into the console's memory now, so a
  // savestate taken between frames holds it: a state loaded later rebuilds the
  // renderer from that memory, and must find what a run that never stopped
  // would have.
  if (g_gl)
    system.GPU().Renderer().Rasterizer()->FlushAll();
}

void StateLoaded()
{
  g_stateLoaded = true;
}

const char* Renderer()
{
  return g_gl ? "opengl" : "software";
}

bool InputWasRead()
{
  return g_inputRead;
}

bool ButtonActive(int i)
{
  if (i < 0 || i >= kButtons)
    return false;
  if (i == 12 || i == 13)  // ZL, ZR: a New 3DS's
    return g_machine.new3ds;
  return true;
}

bool AxisActive(int i)
{
  if (i < 0 || i >= kAxes)
    return false;
  if (i == kCStickX || i == kCStickY)
    return g_machine.new3ds;
  if (i >= kAccelX)
    return g_machine.motion;
  return true;
}

void SetButton(int i, bool on)
{
  if (i >= 0 && i < kButtons)
    g_buttons[i] = on;
}

void SetAxis(int i, int32_t value)
{
  if (i >= 0 && i < kAxes)
    g_axes[i] = value;
}

const uint32_t* Video(int* w, int* h)
{
  *w = g_w;
  *h = g_h;
  return g_video;
}

const int16_t* Audio(int* frames)
{
  *frames = static_cast<int>(g_audio.size() / 2);
  return g_audio.data();
}

std::vector<Domain> Domains()
{
  auto& mem = Core::System::GetInstance().Memory();
  std::vector<Domain> d;
  d.push_back({"FCRAM", mem.GetFCRAMPointer(0),
               g_machine.new3ds ? Memory::FCRAM_N3DS_SIZE : Memory::FCRAM_SIZE});
  d.push_back({"VRAM", mem.GetPhysicalPointer(Memory::VRAM_PADDR), Memory::VRAM_SIZE});
  // The two pages the kernel shares with every process: the configuration
  // (firmware version, memory layout) and the shared page (the RTC, the 3D
  // slider, the battery).
  auto& kernel = Core::System::GetInstance().Kernel();
  d.push_back({"Config Memory", reinterpret_cast<uint8_t*>(&kernel.GetConfigMemHandler().GetConfigMem()),
               static_cast<int64_t>(sizeof(ConfigMem::ConfigMemDef))});
  d.push_back({"Shared Page", reinterpret_cast<uint8_t*>(&kernel.GetSharedPageHandler().GetSharedPage()),
               static_cast<int64_t>(sizeof(SharedPage::SharedPageDef))});
  return d;
}

std::vector<SaveFile> ExportSaveData()
{
  std::vector<SaveFile> out;
  for (const auto& path : ChimeraFSHost::Files("/user/sdmc"))
  {
    SaveFile f;
    f.name = path.substr(std::strlen("/user/"));
    if (ChimeraFSHost::ReadFile(path, f.bytes))
      out.push_back(std::move(f));
  }
  return out;
}
}  // namespace ChimeraAzahar

// The patch series' hooks (see patches 0006 and 0008).
void Chimera_AudioSamples(const s16* data, std::size_t num_samples)
{
  ChimeraAzahar::g_audio.insert(ChimeraAzahar::g_audio.end(), data, data + num_samples * 2);
}

void Chimera_InputRead()
{
  ChimeraAzahar::g_inputRead = true;
}
