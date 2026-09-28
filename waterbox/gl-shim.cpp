// The guest's end of the GPU bridge: Azahar's OpenGL renderer drawing on a real
// GPU outside the sandbox.
//
// Azahar loads every GL entry point through glad's loader, which is exactly the
// seam the bridge wants: each name resolves to a generated wrapper
// (generated-gl/gl-bridge-guest.cpp) that packs the call's arguments and hands
// them to the host through the sandbox's one callback. What the renderer
// believes about the GPU - version, extensions - is what the driver said,
// because glGetString crosses the bridge like everything else.
//
// WHAT THIS COSTS: the GPU is outside the sandbox. It is outside the savestate,
// outside this core's determinism, and different on every machine - and on a
// 3DS the GPU's pictures go back into the console's memory whenever the game
// reads them. A run recorded this way replays only on the same driver.
// SPDX-License-Identifier: MIT

#include <cstdint>
#include <cstdio>

#include <glad/glad.h>

#include "gl-bridge.h" /* miniBox source/gl: the shared contract */
#include "gl-shim.h"

static chimera_gl_bridge_fn g_bridge;

extern "C" void chimera_azahar_install_gpu_bridge(uint64_t addr)
{
  const auto fn = reinterpret_cast<chimera_gl_bridge_fn>(static_cast<uintptr_t>(addr));
  if (fn && chimera_gl_install(fn))
    g_bridge = fn;
}

namespace ChimeraGL
{
bool Present()
{
  return g_bridge != nullptr;
}

bool Load()
{
  // A name the bridge carries no wrapper for comes back null, which is what a
  // driver answers for a call it does not have; glad then leaves that entry
  // point unset, and the 4.3 core set Azahar needs is all on the list.
  return gladLoadGLLoader([](const char* name) -> void* { return chimera_gl_lookup(name); }) != 0;
}

uint64_t ContextId()
{
  return g_bridge ? chimera_gl_context_id() : 0;
}
}  // namespace ChimeraGL
