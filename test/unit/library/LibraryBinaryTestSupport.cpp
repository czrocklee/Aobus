// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "LibraryBinaryTestSupport.h"

#include <ao/utility/ByteView.h>

#include <cstddef>
#include <string_view>
#include <vector>

namespace ao::library::test
{
  void appendString(std::vector<std::byte>& payload, std::string_view str)
  {
    payload.insert_range(payload.end(), utility::bytes::view(str));
    payload.push_back(std::byte{'\0'});
  }
} // namespace ao::library::test
