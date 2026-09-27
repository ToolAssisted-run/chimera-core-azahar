// The driver's side of chimera-fs.cpp: putting the project's files into the
// machine's filesystem, and taking save data out of it.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ChimeraFSHost
{
/// Serve a mounted project file (by its name in the sandbox) read-only at
/// `path`, through one descriptor opened now. Parents are created.
bool MountFile(const std::string& path, const char* host_name);
/// Put bytes at `path` as the machine's own file. Parents are created.
bool WriteFile(const std::string& path, const void* bytes, size_t size);
/// Every file below a folder, sorted, as full paths.
std::vector<std::string> Files(const std::string& under);
/// A whole file's bytes.
bool ReadFile(const std::string& path, std::vector<uint8_t>& out);
}  // namespace ChimeraFSHost
