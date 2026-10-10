// The OpenGL renderer's surfaces, kept where a savestate holds them.
//
// Above the console's own resolution a surface the GPU draws into is a
// texture on the card and nowhere else: the console's memory gets it scaled
// down at every frame's end, and a renderer made again after a load reads that
// back up - other pixels than the ones that were drawn. A load then gives
// another run (chimera issue 223).
//
// So before a state is taken (the StateSaving export) every surface the cache
// has registered is written into a block of the core's own memory: what it is,
// which parts of it are valid, and its pixels at the size they have on the
// card. After a load the cache, made again, is given the same surfaces in the
// order they were registered in. All of them, textures at the console's size
// too: which surfaces a cache holds decides what it does next, and one rebuilt
// from the larger ones only was a different cache.
//
// The block is laid out to change little from one state to the next, because
// a state is stored as what changed:
//   - a surface keeps its place for as long as it stays registered; a new one
//     takes the first gap it fits;
//   - a surface whose modification tick has not moved is not read again;
//   - what is read is compared with what the block holds, a page at a time,
//     and only pages that differ are written.
//
// Nothing here allocates: taking a state must leave the machine as it would
// have been had none been taken, the heap included. The work lists live in a
// scratch block no state carries.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "azahar-driver.h"
#include "azahar-surfaces.h"

#include "core/core.h"
#include "video_core/gpu.h"
#include "video_core/renderer_base.h"
#include "video_core/renderer_opengl/gl_rasterizer.h"

namespace ChimeraAzahar
{
namespace
{
constexpr uint64_t kMagic = 0x3153465253484341ull;  // "ACHSRFS1"
constexpr size_t kPage = 4096;
// the block: a header and the directory, then the pixels
constexpr size_t kDirectoryBytes = 1 << 20;
// the scratch block: the surfaces found, the places taken, the directory being
// written, then rows read from the card
constexpr size_t kMostSurfaces = 8192;
constexpr size_t kListBytes = kMostSurfaces * sizeof(void*);
constexpr size_t kTakenBytes = kMostSurfaces * 2 * sizeof(uint64_t);
constexpr size_t kMinScratch = kListBytes + kTakenBytes + kMostSurfaces + kDirectoryBytes + (1 << 20);

struct Header
{
  uint64_t magic;
  uint64_t serial;     // the cache's last registration number
  uint32_t count;      // entries in the directory
  uint32_t bytes;      // of the directory
  uint64_t left_out;   // surfaces the block had no room for
};

// One surface. `invalid` pairs of addresses follow it.
struct Entry
{
  uint64_t serial;
  uint64_t tick;
  uint64_t offset;  // of its pixels, from the block's start (0: it has none)
  uint64_t bytes;   // of its pixels: every level, rows tightly packed
  uint32_t flags;
  uint32_t fill_size;
  uint8_t fill_data[4];
  uint32_t invalid;
  alignas(8) uint8_t params[(sizeof(VideoCore::SurfaceParams) + 7) & ~size_t{7}];
};

uint8_t* g_block = nullptr;
size_t g_blockBytes = 0;
uint8_t* g_scratch = nullptr;
size_t g_scratchBytes = 0;
bool g_saidFull = false;

size_t EntryBytes(const Entry* e)
{
  return sizeof(Entry) + static_cast<size_t>(e->invalid) * 2 * sizeof(PAddr);
}

OpenGL::RasterizerCache& Cache()
{
  auto* rasterizer = Core::System::GetInstance().GPU().Renderer().Rasterizer();
  return static_cast<OpenGL::RasterizerOpenGL*>(rasterizer)->ChimeraCache();
}

uint64_t PixelBytes(const OpenGL::Surface& s)
{
  const uint64_t bpp = s.ChimeraBytesPerPixel();
  if (bpp == 0 || s.texture_type != VideoCore::TextureType::Texture2D)
    return 0;
  uint64_t n = 0;
  for (uint32_t level = 0; level < s.levels; level++)
    n += static_cast<uint64_t>(s.ChimeraWidth(level)) * s.ChimeraHeight(level) * bpp;
  return n;
}

uint64_t PageUp(uint64_t n)
{
  return (n + kPage - 1) & ~static_cast<uint64_t>(kPage - 1);
}

// Writes only the pages that differ: a page written is a page in the next
// state's delta, whatever it was written with.
void Put(uint8_t* to, const uint8_t* from, size_t n)
{
  while (n)
  {
    const size_t chunk = std::min(n, kPage - (reinterpret_cast<uintptr_t>(to) & (kPage - 1)));
    if (std::memcmp(to, from, chunk) != 0)
      std::memcpy(to, from, chunk);
    to += chunk;
    from += chunk;
    n -= chunk;
  }
}

void ReadPixels(OpenGL::Surface& s, uint8_t* to, uint8_t* rows, size_t rowsBytes)
{
  const uint32_t bpp = s.ChimeraBytesPerPixel();
  for (uint32_t level = 0; level < s.levels; level++)
  {
    const uint32_t w = s.ChimeraWidth(level), h = s.ChimeraHeight(level);
    const size_t row = static_cast<size_t>(w) * bpp;
    const uint32_t strip = static_cast<uint32_t>(std::max<size_t>(1, rowsBytes / row));
    for (uint32_t y = 0; y < h; y += strip)
    {
      const uint32_t n = std::min(strip, h - y);
      s.ChimeraRead(level, y, n, rows, n * row);
      Put(to, rows, n * row);
      to += n * row;
    }
  }
}

struct Taken
{
  uint64_t offset, bytes;
};

// The first gap between the places taken (sorted by offset) that holds
// `bytes`; the place is taken. 0: none.
uint64_t Take(Taken* taken, size_t& count, uint64_t bytes)
{
  uint64_t at = kDirectoryBytes;
  size_t i = 0;
  for (; i < count; i++)
  {
    if (taken[i].offset - at >= bytes)
      break;
    at = taken[i].offset + taken[i].bytes;
  }
  if (i == count && g_blockBytes - at < bytes)
    return 0;
  std::memmove(taken + i + 1, taken + i, (count - i) * sizeof(Taken));
  taken[i] = {at, bytes};
  count++;
  return at;
}
}  // namespace

void SurfaceMemory(void* block, size_t blockBytes, void* scratch, size_t scratchBytes)
{
  if (!block || !scratch || scratchBytes < kMinScratch || blockBytes < 2 * kDirectoryBytes)
    return;
  g_block = static_cast<uint8_t*>(block);
  g_blockBytes = blockBytes;
  g_scratch = static_cast<uint8_t*>(scratch);
  g_scratchBytes = scratchBytes;
}

bool SurfacesKept()
{
  return g_block != nullptr;
}

void KeepSurfaces()
{
  if (!g_block)
    return;
  auto& cache = Cache();

  // the scratch block's parts
  auto** list = reinterpret_cast<OpenGL::Surface**>(g_scratch);
  auto* taken = reinterpret_cast<Taken*>(g_scratch + kListBytes);
  uint8_t* read = g_scratch + kListBytes + kTakenBytes;  // one byte a surface: read its pixels
  uint8_t* dir = read + kMostSurfaces;
  uint8_t* rows = dir + kDirectoryBytes;
  const size_t rowsBytes = g_scratchBytes - (rows - g_scratch);

  // every registered surface, in the order they were registered in
  struct Found
  {
    OpenGL::Surface** list;
    size_t count;
    size_t more;
  } found{list, 0, 0};
  cache.ChimeraForEachRegistered(
      [](void* ctx, OpenGL::Surface& s) {
        auto* f = static_cast<Found*>(ctx);
        if (f->count < kMostSurfaces)
          f->list[f->count++] = &s;
        else
          f->more++;
      },
      &found);
  std::sort(list, list + found.count, [](const OpenGL::Surface* a, const OpenGL::Surface* b) {
    return a->chimera_serial < b->chimera_serial;
  });

  // the directory, each surface in the place it had if it had one
  const auto* was = reinterpret_cast<const Header*>(g_block);
  const bool hadAny = was->magic == kMagic;
  const uint8_t* old = g_block + sizeof(Header);
  uint32_t oldLeft = hadAny ? was->count : 0;

  auto* header = reinterpret_cast<Header*>(dir);
  std::memset(header, 0, sizeof *header);
  uint8_t* out = dir + sizeof(Header);
  size_t count = 0, takenCount = 0;
  uint64_t leftOut = found.more;
  for (size_t i = 0; i < found.count; i++)
  {
    OpenGL::Surface& s = *list[i];
    uint32_t invalid = 0;
    for (auto it = s.invalid_regions.begin(); it != s.invalid_regions.end(); ++it)
      invalid++;
    const size_t need = sizeof(Entry) + static_cast<size_t>(invalid) * 2 * sizeof(PAddr);
    if (static_cast<size_t>(out - dir) + need > kDirectoryBytes)
    {
      leftOut += found.count - i;
      break;
    }
    auto* e = reinterpret_cast<Entry*>(out);
    std::memset(e, 0, sizeof *e);
    e->serial = s.chimera_serial;
    e->tick = s.ModificationTick();
    e->bytes = PixelBytes(s);
    e->flags = static_cast<uint32_t>(s.flags);
    e->fill_size = s.fill_size;
    std::memcpy(e->fill_data, s.fill_data.data(), 4);
    e->invalid = invalid;
    std::memcpy(e->params, static_cast<const VideoCore::SurfaceParams*>(&s), sizeof(VideoCore::SurfaceParams));
    auto* pair = reinterpret_cast<PAddr*>(e + 1);
    for (auto it = s.invalid_regions.begin(); it != s.invalid_regions.end(); ++it)
    {
      *pair++ = it->lower();
      *pair++ = it->upper();
    }

    // its place before, if this registration was in the block
    read[count] = e->bytes != 0;
    while (oldLeft && reinterpret_cast<const Entry*>(old)->serial < e->serial)
    {
      old += EntryBytes(reinterpret_cast<const Entry*>(old));
      oldLeft--;
    }
    if (oldLeft && e->bytes)
    {
      const auto* o = reinterpret_cast<const Entry*>(old);
      if (o->serial == e->serial && o->bytes == e->bytes && o->offset)
      {
        e->offset = o->offset;
        read[count] = o->tick != e->tick;
        taken[takenCount++] = {o->offset, PageUp(o->bytes)};
      }
    }
    list[count++] = &s;
    out += need;
  }

  // a place for each surface that has none: the first gap it fits
  std::sort(taken, taken + takenCount, [](const Taken& a, const Taken& b) { return a.offset < b.offset; });
  bool fits = true;
  {
    uint8_t* p = dir + sizeof(Header);
    for (size_t i = 0; i < count && fits; i++)
    {
      auto* e = reinterpret_cast<Entry*>(p);
      if (e->bytes && !e->offset)
        fits = (e->offset = Take(taken, takenCount, PageUp(e->bytes))) != 0;
      p += EntryBytes(e);
    }
  }
  if (!fits)
  {
    // no gap is large enough: every surface moves up against the one before
    // it, and is read again; what still has no room is left out
    uint8_t* p = dir + sizeof(Header);
    uint64_t at = kDirectoryBytes;
    size_t kept = 0;
    for (; kept < count; kept++)
    {
      auto* e = reinterpret_cast<Entry*>(p);
      if (e->bytes)
      {
        if (g_blockBytes - at < PageUp(e->bytes))
          break;
        e->offset = at;
        at += PageUp(e->bytes);
        read[kept] = 1;
      }
      p += EntryBytes(e);
    }
    leftOut += count - kept;
    count = kept;
    out = p;
  }

  // the pixels that changed
  {
    uint8_t* p = dir + sizeof(Header);
    for (size_t i = 0; i < count; i++)
    {
      auto* e = reinterpret_cast<Entry*>(p);
      if (read[i])
        ReadPixels(*list[i], g_block + e->offset, rows, rowsBytes);
      p += EntryBytes(e);
    }
  }

  header->magic = kMagic;
  header->serial = cache.chimera_serial;
  header->count = static_cast<uint32_t>(count);
  header->bytes = static_cast<uint32_t>(out - dir);
  header->left_out = leftOut;
  Put(g_block, dir, out - dir);

  if (leftOut && !g_saidFull)
  {
    g_saidFull = true;
    fprintf(stderr,
            "[azahar] %llu surfaces did not fit in the memory a state keeps them in: a load "
            "draws them again from the console's memory, at the console's resolution\n",
            static_cast<unsigned long long>(leftOut));
  }
}

void PutBackSurfaces()
{
  if (!g_block)
    return;
  const auto* header = reinterpret_cast<const Header*>(g_block);
  if (header->magic != kMagic)
    return;
  auto& cache = Cache();
  const uint8_t* p = g_block + sizeof(Header);
  for (uint32_t i = 0; i < header->count; i++)
  {
    const auto* e = reinterpret_cast<const Entry*>(p);
    VideoCore::ChimeraSurfaceRecord record;
    std::memcpy(static_cast<void*>(&record.params), e->params, sizeof(VideoCore::SurfaceParams));
    record.flags = static_cast<VideoCore::SurfaceFlagBits>(e->flags);
    record.fill_size = e->fill_size;
    std::memcpy(record.fill_data.data(), e->fill_data, 4);
    record.serial = e->serial;
    record.tick = e->tick;
    record.invalid = reinterpret_cast<const PAddr*>(e + 1);
    record.invalid_count = e->invalid;
    OpenGL::Surface& s = cache.ChimeraAdopt(record);
    if (e->bytes && e->offset)
    {
      const uint8_t* pixels = g_block + e->offset;
      const uint32_t bpp = s.ChimeraBytesPerPixel();
      for (uint32_t level = 0; level < s.levels; level++)
      {
        s.ChimeraWrite(level, pixels);
        pixels += static_cast<size_t>(s.ChimeraWidth(level)) * s.ChimeraHeight(level) * bpp;
      }
    }
    p += EntryBytes(e);
  }
  cache.chimera_serial = header->serial;
}
}  // namespace ChimeraAzahar
