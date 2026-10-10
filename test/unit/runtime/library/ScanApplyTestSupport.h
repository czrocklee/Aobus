// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "test/unit/library/TrackTestSupport.h"
#include <ao/CoreIds.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

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

  /// Writes a metadata-readable FLAC fixture with a nonempty synthetic payload
  /// for identity hashing; the scan does not decode these bytes as audio
  /// frames. Identical comments and sample counts produce identical bytes.
  void writeScanFlacMetadataFixture(std::filesystem::path const& path,
                                    std::vector<std::string> const& comments,
                                    std::uint64_t totalSamples = 44100);

  /// Reads one stored track's complete metadata spec through its cold record.
  library::test::TrackSpec storedTrackSpec(library::MusicLibrary& library, TrackId trackId);

  void requirePrepared(ScanApplyOperation& operation);

  /// Revalidates a prepared operation; it stays ready for mutation only without failures.
  void requireRevalidation(ScanApplyOperation& operation, std::int32_t staleCount, std::int32_t failureCount);
} // namespace ao::rt::test
