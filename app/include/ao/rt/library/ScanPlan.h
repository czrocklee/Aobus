// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/library/AudioIdentity.h>
#include <ao/utility/Hash128.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ao::rt
{
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
    return library::hasAudioIdentity(item.audioPayloadLength, item.audioSignature);
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

  enum class AudioIdentityPolicy : std::uint8_t
  {
    Eager,
    DeferNew
  };

  struct ScanApplyOptions final
  {
    AudioIdentityPolicy audioIdentityPolicy = AudioIdentityPolicy::Eager;
  };

  /**
   * ScanFailure - A single failure surfaced while applying a scan plan.
   *
   * Only failures are reported; the happy path (inserted/updated/unchanged/
   * missing) is not, and processed TrackIds are returned in bulk via
   * ScanApplyResult::processedIds. Every field is a view valid only for the
   * duration of the callback invocation; copy out anything that must outlive it.
   */
  struct ScanFailure final
  {
    std::string_view uri;     // item being processed (empty when not item-scoped)
    std::string_view stage;   // operation that failed, e.g. "open"
    std::string_view message; // raw error detail
  };

  enum class ScanApplyProgressStage : std::uint8_t
  {
    Updating,
    Fingerprinting,
  };

  struct ScanApplyProgress final
  {
    std::filesystem::path path{};
    std::int32_t itemIndex = 0;
    ScanApplyProgressStage stage = ScanApplyProgressStage::Updating;
    double itemFraction = 0.0;
  };

  /**
   * Result of applying a scan plan after the batch transaction commits.
   *
   * Per-item failures are counted here and streamed live through the apply
   * failure callback; transaction-level failures use the Result error channel
   * instead, so this value is only meaningful on success.
   */
  struct ScanApplyResult final
  {
    std::vector<TrackId> processedIds;
    std::int32_t relinkedCount = 0;
    std::int32_t missingCount = 0;
    std::int32_t failureCount = 0;
    bool cancelled = false;
  };
} // namespace ao::rt
