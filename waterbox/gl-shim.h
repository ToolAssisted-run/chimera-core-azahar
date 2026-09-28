// The driver's view of the GPU bridge (gl-shim.cpp).
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

extern "C" void chimera_azahar_install_gpu_bridge(uint64_t addr);

namespace ChimeraGL
{
/// A host handed a bridge over (SetGpuBridge, before Init).
bool Present();
/// Point glad's entry points at the bridge's wrappers.
bool Load();
/// Which context the calls land on; 0 when it cannot tell (see gl-bridge.h).
uint64_t ContextId();
}  // namespace ChimeraGL
