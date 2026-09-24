// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>

#include <cstdint>
#include <filesystem>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::rt
{
  class ScanApplyOperation;
}

namespace ao::rt::test
{
  /// Replaces @p target and advances its timestamp so the next scan sees a changed file.
  void replaceFile(std::filesystem::path const& target, std::filesystem::path const& source);

  TrackId importOne(library::MusicLibrary& library);

  void requirePrepared(ScanApplyOperation& operation);

  /// Revalidates a prepared operation; it stays ready for mutation only without failures.
  void requireRevalidation(ScanApplyOperation& operation, std::int32_t staleCount, std::int32_t failureCount);
} // namespace ao::rt::test
