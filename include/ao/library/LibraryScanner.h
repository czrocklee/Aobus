// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/utility/Fnv1a.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace ao::library
{
  class MusicLibrary;

  enum class ScanClassification : std::uint8_t
  {
    New,
    Changed,
    Moved,
    Missing,
    Unchanged,
    Error
  };

  struct ScanItem final
  {
    std::string uri;
    std::string oldUri;
    std::filesystem::path fullPath;
    ScanClassification classification = ScanClassification::Error;
    std::uint64_t fileSize = 0;
    std::uint64_t mtime = 0;
    std::uint64_t audioPayloadLength = 0;
    utility::Hash128 audioSignature = {};
    TrackId trackId = kInvalidTrackId;
    std::string errorMessage = {};
  };

  inline bool hasAudioIdentity(ScanItem const& item) noexcept
  {
    return item.audioPayloadLength != 0 && item.audioSignature != utility::Hash128{};
  }

  struct ScanPlan final
  {
    std::vector<ScanItem> items;

    std::size_t count(ScanClassification classification) const
    {
      std::size_t count = 0;

      for (auto const& item : items)
      {
        if (item.classification == classification)
        {
          ++count;
        }
      }

      return count;
    }
  };

  /**
   * Result of applying a scan plan after the batch transaction commits.
   *
   * Per-item failures are counted here and streamed live through
   * ScanPlanExecutor::ItemFailureCallback; transaction-level failures use the
   * Result error channel instead, so this value is only meaningful on success.
   */
  struct ScanApplyResult final
  {
    std::vector<TrackId> processedIds;
    std::int32_t relinkedCount = 0;
    std::int32_t missingCount = 0;
    std::int32_t failureCount = 0;
    bool cancelled = false;
  };

  class LibraryScanner final
  {
  public:
    using ProgressCallback = std::move_only_function<void(std::filesystem::path const& currentPath)>;

    explicit LibraryScanner(MusicLibrary& ml);

    /**
     * Scans the music root recursively and compares with the manifest.
     *
     * Per-file problems (an unreadable entry, a directory we cannot enter) are
     * carried in-band as `ScanClassification::Error` items so the rest of the
     * plan still applies. The `Result` error channel is reserved for failures
     * that prevent any plan from being built at all: a music root that does not
     * exist (`NotFound`) or a filesystem walk that cannot even start
     * (`IoError`). An empty plan is a valid success (an empty library).
     */
    Result<ScanPlan> buildPlan(ProgressCallback progress = nullptr);

  private:
    MusicLibrary& _ml;
  };
} // namespace ao::library
