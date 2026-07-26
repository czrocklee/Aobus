// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <cstddef>
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

  enum class LibraryTaskCompletionStatus : std::uint8_t
  {
    Succeeded,
    CompletedWithIssues,
    Failed,
    Cancelled,
  };

  struct LibraryTaskCompleted final
  {
    LibraryTaskCompletionStatus status = LibraryTaskCompletionStatus::Succeeded;
    std::size_t affectedCount = 0;
  };
} // namespace ao::rt
