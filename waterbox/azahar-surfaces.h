// The OpenGL renderer's surfaces, kept where a savestate holds them (see
// azahar-surfaces.cpp). The driver's own: nothing outside it calls these.
#pragma once

namespace ChimeraAzahar
{
/// Whether there is memory to keep them in (SurfaceMemory was given some).
bool SurfacesKept();
/// Writes every surface the renderer's cache has registered into the block.
/// The renderer's objects must be this GL context's.
void KeepSurfaces();
/// Gives a cache that was just made the surfaces the block holds.
void PutBackSurfaces();
}  // namespace ChimeraAzahar
