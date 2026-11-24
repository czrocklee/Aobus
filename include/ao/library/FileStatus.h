// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>

namespace ao::library
{
  // Physical availability of a track. Values are persisted in the file manifest.
  enum class FileStatus : std::uint8_t
  {
    Available = 0,
    Missing = 1,
    Error = 2
  };
} // namespace ao::library
