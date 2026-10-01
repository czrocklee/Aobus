// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/FileTimestamp.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <variant>

namespace ao::rt
{
  using TrackFieldDuration = std::chrono::milliseconds;

  // Absence is std::monostate. A present modification time is a Unix/POSIX
  // FileTimestamp whose fraction is preserved for exact equality.
  using TrackFieldRawValue = std::variant<std::monostate,
                                          std::string,
                                          std::uint16_t,
                                          std::uint32_t,
                                          std::uint64_t,
                                          TrackFieldDuration,
                                          FileTimestamp>;
} // namespace ao::rt
