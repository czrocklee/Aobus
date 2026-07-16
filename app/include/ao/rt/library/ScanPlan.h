// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/AudioIdentity.h>
#include <ao/utility/Hash128.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::rt
{
  class ScanApplyOperation;
  class ScanPlanBuilder;

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
    std::string oldUri = {};
    std::filesystem::path fullPath = {};
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

  class ScanPlan final
  {
  public:
    ~ScanPlan() = default;

    ScanPlan(ScanPlan const&) = delete;
    ScanPlan& operator=(ScanPlan const&) = delete;
    ScanPlan(ScanPlan&& other) noexcept;
    ScanPlan& operator=(ScanPlan&& other) noexcept;

    std::span<ScanItem const> items() const noexcept { return _items; }
    std::size_t size() const noexcept { return _items.size(); }
    bool empty() const noexcept { return _items.empty(); }

    std::size_t count(ScanClassification classification) const noexcept
    {
      std::size_t count = 0;

      for (auto const& item : _items)
      {
        if (item.classification == classification)
        {
          ++count;
        }
      }

      return count;
    }

    /**
     * Builds a single-item explicit relink plan from two unresolved items.
     *
     * A successful derivation consumes the source plan so its library and
     * revision binding cannot be separated from the derived operation. The
     * destination must be New, the source must be Missing, and both planned
     * audio identities must match.
     */
    Result<ScanPlan> makeRelinkPlan(std::string_view oldUri, std::string_view newUri) &&;

  private:
    ScanPlan(std::array<std::byte, 16> libraryId, std::uint64_t libraryRevision, std::vector<ScanItem> items) noexcept;

    std::array<std::byte, 16> _libraryId{};
    std::uint64_t _libraryRevision = 0;
    std::vector<ScanItem> _items;
    bool _executable = true;

    friend class ScanApplyOperation;
    friend class ScanPlanBuilder;
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
   * missing) is not, and changed TrackIds are returned in bulk via the
   * categorized ScanApplyResult vectors. Every field is a view valid only for the
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
   * Result of applying a scan plan after its terminal no-op or commit decision.
   *
   * Per-item failures are counted here and streamed live through the apply
   * failure callback; transaction-level failures use the Result error channel
   * instead, so this value is only meaningful on success.
   */
  struct ScanApplyResult final
  {
    std::uint64_t libraryRevision = 0;
    std::vector<TrackId> insertedIds;
    std::vector<TrackId> mutatedIds;
    std::vector<TrackId> relinkedIds;
    std::int32_t missingCount = 0;
    std::int32_t failureCount = 0;
  };
} // namespace ao::rt
