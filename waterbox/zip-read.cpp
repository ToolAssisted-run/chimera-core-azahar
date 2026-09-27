// A whole-entry zip reader for the Save data slot: the .zip Export Save Data
// writes is small (a game's save archive and extra data), so every entry is
// unpacked at once. Stored and deflated entries; the inflater is CryptoPP's,
// which Azahar already carries - no zlib is needed for this one job.
// SPDX-License-Identifier: MIT
#include "zip-read.h"

#include <cstdio>
#include <cstring>

#include <cryptopp/filters.h>
#include <cryptopp/zinflate.h>

namespace
{
uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p)
{
  return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24));
}

bool ReadAll(const char* name, std::vector<uint8_t>& out)
{
  FILE* f = fopen(name, "rb");
  if (!f)
    return false;
  uint8_t chunk[1 << 16];
  size_t n;
  while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
    out.insert(out.end(), chunk, chunk + n);
  fclose(f);
  return true;
}
}  // namespace

bool ZipReadAll(const char* host_name, std::vector<ZipFile>& files, std::string& error)
{
  std::vector<uint8_t> z;
  if (!ReadAll(host_name, z))
  {
    error = "cannot read it";
    return false;
  }
  // the end of central directory record: the last one in the final 64 KiB + 22
  if (z.size() < 22)
  {
    error = "too short to be a zip";
    return false;
  }
  size_t eocd = std::string::npos;
  const size_t lowest = z.size() > 65557 ? z.size() - 65557 : 0;
  for (size_t i = z.size() - 22 + 1; i-- > lowest;)
    if (rd32(&z[i]) == 0x06054b50)
    {
      eocd = i;
      break;
    }
  if (eocd == std::string::npos)
  {
    error = "no zip directory in it";
    return false;
  }
  const uint16_t count = rd16(&z[eocd + 10]);
  size_t at = rd32(&z[eocd + 16]);
  for (uint16_t k = 0; k < count; k++)
  {
    if (at + 46 > z.size() || rd32(&z[at]) != 0x02014b50)
    {
      error = "a damaged zip directory";
      return false;
    }
    const uint16_t method = rd16(&z[at + 10]);
    const uint32_t csize = rd32(&z[at + 20]), size = rd32(&z[at + 24]);
    const uint16_t nlen = rd16(&z[at + 28]), xlen = rd16(&z[at + 30]), clen = rd16(&z[at + 32]);
    const uint32_t local = rd32(&z[at + 42]);
    if (csize == 0xFFFFFFFFu || size == 0xFFFFFFFFu || local == 0xFFFFFFFFu)
    {
      error = "a zip64 entry (a save is never that large)";
      return false;
    }
    std::string name(reinterpret_cast<const char*>(&z[at + 46]), nlen);
    at += 46 + nlen + xlen + clen;
    if (name.empty() || name.back() == '/')
      continue;  // a folder: the files under it make it
    if (local + 30 > z.size() || rd32(&z[local]) != 0x04034b50)
    {
      error = "a damaged entry '" + name + "'";
      return false;
    }
    const size_t data = local + 30 + rd16(&z[local + 26]) + rd16(&z[local + 28]);
    if (data + csize > z.size())
    {
      error = "a truncated entry '" + name + "'";
      return false;
    }
    ZipFile f{name, {}};
    if (method == 0)
      f.bytes.assign(z.begin() + data, z.begin() + data + csize);
    else if (method == 8)
    {
      try
      {
        std::string outs;
        CryptoPP::StringSource src(&z[data], csize, true,
                                   new CryptoPP::Inflator(new CryptoPP::StringSink(outs)));
        f.bytes.assign(outs.begin(), outs.end());
      }
      catch (const std::exception& e)
      {
        error = "entry '" + name + "' does not inflate: " + e.what();
        return false;
      }
    }
    else
    {
      error = "entry '" + name + "' is compressed a way this core does not read (method " +
              std::to_string(method) + ")";
      return false;
    }
    if (f.bytes.size() != size)
    {
      error = "entry '" + name + "' unpacks to the wrong size";
      return false;
    }
    files.push_back(std::move(f));
  }
  return true;
}
