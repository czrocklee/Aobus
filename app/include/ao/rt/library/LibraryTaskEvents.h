// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/utility/StrongType.h>

#include <cstdint>
#include <string>

namespace ao::rt
{
  using LibraryTaskProgressId = utility::StrongType<std::uint64_t, struct LibraryTaskProgressIdTag>;

  inline constexpr auto kInvalidLibraryTaskProgressId = LibraryTaskProgressId{0};

  enum class LibraryTaskProgressKind : std::uint8_t
  {
    Scanning,
    Updating,
    Fingerprinting,
    IndexingAudioIdentity,
    PreparingImport,
    Importing,
    Exporting,
  };

  struct LibraryTaskProgressUpdated final
  {
    LibraryTaskProgressId id = kInvalidLibraryTaskProgressId;
    LibraryTaskProgressKind kind = LibraryTaskProgressKind::Scanning;
    double fraction = 0.0;
    std::string subject{};
  };

  struct LibraryTaskProgressFinished final
  {
    LibraryTaskProgressId id = kInvalidLibraryTaskProgressId;
  };
} // namespace ao::rt
