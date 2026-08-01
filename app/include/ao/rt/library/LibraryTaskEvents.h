// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <string>

namespace ao::rt
{
  enum class LibraryTaskProgressKind : std::uint8_t
  {
    Scanning,
    Updating,
    Fingerprinting,
    IndexingAudioIdentity,
  };

  struct LibraryTaskProgressUpdated final
  {
    LibraryTaskProgressKind kind = LibraryTaskProgressKind::Scanning;
    double fraction = 0.0;
    std::string subject{};
  };
} // namespace ao::rt
