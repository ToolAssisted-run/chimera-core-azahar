// The Save data slot's .zip, unpacked whole (see zip-read.cpp).
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct ZipFile
{
  std::string name;  // as stored: '/' separated, no leading '/'
  std::vector<uint8_t> bytes;
};

/// Every file in the zip mounted as `host_name`. False with `error` set.
bool ZipReadAll(const char* host_name, std::vector<ZipFile>& files, std::string& error);
