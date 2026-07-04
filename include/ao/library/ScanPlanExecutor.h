// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/LibraryScanner.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/lmdb/Transaction.h>
#include <ao/utility/Fnv1a.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace ao::tag
{
  class TagFile;
}

namespace ao::library
{
  class MusicLibrary;
  struct ScanPlan;
  struct ScanItem;
  class DictionaryStore;

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

  /**
   * ScanPlanExecutor - Applies a ScanPlan to the MusicLibrary database.
   *
   * This class focus exclusively on reconciling the filesystem scan results
   * with the database state.
   */
  class ScanPlanExecutor final
  {
  public:
    enum class ProgressStage : std::uint8_t
    {
      Updating,
      Fingerprinting
    };

    struct Progress final
    {
      std::filesystem::path path{};
      std::int32_t itemIndex = 0;
      ProgressStage stage = ProgressStage::Updating;
      double itemFraction = 0.0;
    };

    using ProgressCallback = std::move_only_function<void(Progress const& progress)>;
    using ItemFailureCallback = std::move_only_function<void(ScanFailure const& failure)>;

    ScanPlanExecutor(MusicLibrary& ml,
                     ScanPlan plan,
                     ProgressCallback progressCallback,
                     ItemFailureCallback itemFailureCallback);

    ~ScanPlanExecutor() = default;

    ScanPlanExecutor(ScanPlanExecutor const&) = delete;
    ScanPlanExecutor& operator=(ScanPlanExecutor const&) = delete;
    ScanPlanExecutor(ScanPlanExecutor&&) = delete;
    ScanPlanExecutor& operator=(ScanPlanExecutor&&) = delete;

    // Run execution in the current thread - must be called from background thread.
    // Respects cancellation via stop_token.
    Result<ScanApplyResult> run(std::stop_token stopToken = {});

    std::size_t fileCount() const;

  private:
    struct AudioFingerprint final
    {
      utility::Hash128 signature;
      std::uint64_t payloadLength = 0;
    };

    void processItem(std::size_t itemIndex,
                     lmdb::WriteTransaction& txn,
                     TrackStore::Writer& trackWriter,
                     FileManifestStore::Writer& manifestWriter,
                     DictionaryStore& dict,
                     std::stop_token stopToken);

    bool processSkips(ScanItem const& item);

    void reportProgress(ScanItem const& item, std::size_t itemIndex, ProgressStage stage, double itemFraction);

    void reportFailure(std::string_view uri, std::string_view stage, std::string_view message);

    void processMissing(ScanItem const& item, lmdb::WriteTransaction& txn, FileManifestStore::Writer& manifestWriter);

    bool processChanged(ScanItem const& item,
                        lmdb::WriteTransaction& txn,
                        TrackStore::Writer& trackWriter,
                        FileManifestStore::Writer& manifestWriter,
                        DictionaryStore& dict,
                        TrackBuilder& builder,
                        AudioFingerprint const& fingerprint);

    bool processMoved(ScanItem const& item,
                      lmdb::WriteTransaction& txn,
                      TrackStore::Writer& trackWriter,
                      FileManifestStore::Writer& manifestWriter,
                      DictionaryStore& dict,
                      TrackBuilder& builder,
                      AudioFingerprint const& fingerprint);

    void processNew(ScanItem const& item,
                    lmdb::WriteTransaction& txn,
                    TrackStore::Writer& trackWriter,
                    FileManifestStore::Writer& manifestWriter,
                    DictionaryStore& dict,
                    TrackBuilder& builder,
                    AudioFingerprint const& fingerprint);

    std::optional<std::pair<std::unique_ptr<tag::TagFile>, TrackBuilder>> loadTrackBuilder(ScanItem const& item);

    std::optional<AudioFingerprint> cachedAudioFingerprint(ScanItem const& item) const noexcept;

    std::optional<AudioFingerprint> fingerprintAudioPayload(ScanItem const& item,
                                                            tag::TagFile const& tagFile,
                                                            std::size_t itemIndex,
                                                            std::stop_token stopToken);

    std::optional<std::pair<TrackBuilder::PreparedHot, TrackBuilder::PreparedCold>> prepareTrack(
      TrackBuilder const& builder,
      lmdb::WriteTransaction& txn,
      DictionaryStore& dict,
      std::string const& uri);

    bool writeManifest(FileManifestStore::Writer& writer, std::string const& uri, FileManifestBuilder& builder);

    bool updateTrack(TrackStore::Writer& trackWriter,
                     TrackId trackId,
                     std::string const& uri,
                     TrackBuilder::PreparedHot const& hot,
                     TrackBuilder::PreparedCold const& cold);

    std::optional<TrackId> createTrack(TrackStore::Writer& trackWriter,
                                       std::string const& uri,
                                       TrackBuilder::PreparedHot const& hot,
                                       TrackBuilder::PreparedCold const& cold);

    MusicLibrary& _ml;
    std::unique_ptr<ScanPlan> _planPtr;
    ProgressCallback _progressCallback;
    ItemFailureCallback _itemFailureCallback;

    ScanApplyResult _result;
    bool _abortTransaction = false;
  };
} // namespace ao::library
