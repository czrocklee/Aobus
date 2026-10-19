// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/FileTimestamp.h>
#include <ao/library/AudioIdentity.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/TrackLayout.h>
#include <ao/utility/Hash128.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::rt
{
  class ScanApplyOperation;
  class LibraryScan;

  enum class ScanClassification : std::uint8_t
  {
    New,
    Changed,
    MissingSegments,
    Moved,
    Missing,
    Unchanged,
    Error,
    // A report-only entry, such as a CUE sheet that names no supported audio
    // file. It names no library file and is never applied.
    Notice,
  };

  enum class ScanCueDiagnostic : std::uint8_t
  {
    None,
    InvalidEncoding,
    SheetUnreadableOrMalformed,
    AmbiguousSheets,
    UnsupportedCarrier,
    NoMatchingAudio,
    WholeFileAlreadyImported,
    SheetMissingForCarrier,
    LayoutMismatch,
    CarrierFactsMismatch,
  };

  // Structural membership, independent of curated Track metadata.
  struct ScanCarrierMember final
  {
    TrackId trackId = kInvalidTrackId;
    library::TrackSegment segment{};
    bool operator==(ScanCarrierMember const&) const noexcept = default;
  };

  struct ScanSheetEvidence final
  {
    std::string name{};
    std::uint64_t fileSize = 0;
    std::optional<FileTimestamp> optMtime{};
    utility::Hash128 digest{};
    std::string errorMessage{};
    ScanCueDiagnostic cueDiagnostic = ScanCueDiagnostic::None;
    bool operator==(ScanSheetEvidence const&) const noexcept = default;
  };

  struct ScanCueEvidence final
  {
    std::vector<ScanSheetEvidence> general{};
    std::vector<ScanSheetEvidence> extensionSpecific{};
    bool operator==(ScanCueEvidence const&) const noexcept = default;
  };

  /**
   * Persisted physical row and carrier structure observed by the planner.
   * The ScanItem supplies the key and whole-file TrackId. Moved items keep
   * source-row evidence separate from their destination filesystem facts.
   */
  struct ScanManifestEvidence final
  {
    std::uint64_t fileSize = 0;
    // Nullopt when the manifest row stores no modification time; a Unix epoch
    // zero instant is a valid value and stays distinct from absence.
    std::optional<FileTimestamp> optMtime{};
    std::uint64_t audioPayloadLength = 0;
    utility::Hash128 audioSignature = {};
    library::FileStatus status = library::FileStatus::Available;
    std::uint64_t totalFrames = 0;
    std::uint32_t sampleRate = 0;
    std::vector<ScanCarrierMember> members{};
  };

  struct ScanItem final
  {
    std::string uri;
    std::string oldUri = {};
    std::filesystem::path fullPath = {};
    ScanClassification classification = ScanClassification::Error;
    std::uint64_t fileSize = 0;
    // Destination-file POSIX modification time; engaged after a successful
    // inspection, and nullopt for items that never reached one.
    std::optional<FileTimestamp> optMtime{};
    std::uint64_t audioPayloadLength = 0;
    utility::Hash128 audioSignature = {};
    TrackId trackId = kInvalidTrackId;
    std::optional<ScanManifestEvidence> optManifestEvidence = std::nullopt;
    std::string errorMessage = {};
    ScanCueDiagnostic cueDiagnostic = ScanCueDiagnostic::None;
    std::string cueDetail{};
    ScanCueEvidence cueEvidence{};
    std::vector<library::TrackSegment> cueLayout{};
    std::uint64_t totalFrames = 0;
    std::uint32_t sampleRate = 0;
    // True only when totalFrames and sampleRate hold inspected exact media facts.
    bool hasExactMediaFacts = false;
  };

  inline bool hasAudioIdentity(ScanItem const& item) noexcept
  {
    return library::hasAudioIdentity(item.audioPayloadLength, item.audioSignature);
  }

  // A failure leaves its item unapplied and fails the scan.
  inline bool isScanFailure(ScanItem const& item) noexcept
  {
    return item.classification == ScanClassification::Error;
  }

  // A notice accompanies its item's classification: it reports state that a
  // person should settle, but neither blocks the item nor fails the scan.
  inline bool isScanNotice(ScanItem const& item) noexcept
  {
    return !isScanFailure(item) && !item.errorMessage.empty();
  }

  // Admission uses only owned planner evidence; final apply still proves live identity.
  bool isRelinkDestination(ScanItem const& item) noexcept;
  bool canRelinkItems(ScanItem const& source, ScanItem const& destination) noexcept;

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

    std::size_t count(ScanClassification classification) const noexcept;

    /**
     * Builds a single-item explicit relink plan from two unresolved items.
     *
     * A successful derivation consumes the source plan so its library binding,
     * planner provenance, and source evidence stay with the derived operation.
     * The destination must be New or a fingerprinted new file blocked by CUE
     * binding, the source must be Missing, and both planned identities must match.
     */
    Result<ScanPlan> makeRelinkPlan(std::string_view oldUri, std::string_view newUri) &&;

  private:
    ScanPlan(std::array<std::byte, 16> libraryId, std::uint64_t libraryRevision, std::vector<ScanItem> items) noexcept;

    std::array<std::byte, 16> _libraryId{};
    std::uint64_t _libraryRevision = 0;
    std::vector<ScanItem> _items;
    bool _executable = true;

    friend class ScanApplyOperation;
    friend class LibraryScan;
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
   * failure callback. Ordinary evidence that became stale is counted separately
   * and left for a later scan. Transaction-level failures use the Result error
   * channel instead, so this value is only meaningful on success.
   */
  struct ScanApplyResult final
  {
    std::uint64_t libraryRevision = 0;
    std::vector<TrackId> insertedIds;
    std::vector<TrackId> mutatedIds;
    std::vector<TrackId> relinkedIds;
    std::int32_t missingCount = 0;
    std::int32_t staleCount = 0;
    std::int32_t failureCount = 0;
    /// Physical files relinked; one carrier contributes every member id but counts once.
    std::int32_t relinkedFileCount = 0;
  };
} // namespace ao::rt
