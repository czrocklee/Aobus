// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/lmdb/Transaction.h>
#include <ao/rt/library/ScanPlan.h>
#include <ao/utility/Hash128.h>

#include <cstddef>
#include <cstdint>
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
  class DictionaryStore;
}

namespace ao::rt
{
  /**
   * ScanPlanApplier - Applies a ScanPlan to the MusicLibrary database.
   *
   * This class focuses exclusively on reconciling the filesystem scan results
   * with the database state.
   */
  class ScanPlanApplier final
  {
  public:
    ScanPlanApplier(library::MusicLibrary& ml,
                    ScanPlan plan,
                    std::move_only_function<void(ScanApplyProgress const& progress)> progressCallback,
                    std::move_only_function<void(ScanFailure const& failure)> itemFailureCallback,
                    ScanApplyOptions options = {});

    ~ScanPlanApplier() = default;

    ScanPlanApplier(ScanPlanApplier const&) = delete;
    ScanPlanApplier& operator=(ScanPlanApplier const&) = delete;
    ScanPlanApplier(ScanPlanApplier&&) = delete;
    ScanPlanApplier& operator=(ScanPlanApplier&&) = delete;

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

    void applyScanItem(std::size_t itemIndex,
                       lmdb::WriteTransaction& transaction,
                       library::TrackStore::Writer& trackWriter,
                       library::FileManifestStore::Writer& manifestWriter,
                       library::DictionaryStore& dictionary,
                       std::stop_token stopToken);

    bool skipNonActionableItem(ScanItem const& item);

    void reportProgress(ScanItem const& item, std::size_t itemIndex, ScanApplyProgressStage stage, double itemFraction);

    void reportFailure(std::string_view uri, std::string_view stage, std::string_view message);

    void applyMissingItem(ScanItem const& item,
                          lmdb::WriteTransaction& transaction,
                          library::FileManifestStore::Writer& manifestWriter);

    bool tryApplyChangedItem(ScanItem const& item,
                             lmdb::WriteTransaction& transaction,
                             library::TrackStore::Writer& trackWriter,
                             library::FileManifestStore::Writer& manifestWriter,
                             library::DictionaryStore& dictionary,
                             library::TrackBuilder& builder,
                             AudioFingerprint const& fingerprint);

    bool applyMovedItem(ScanItem const& item,
                        lmdb::WriteTransaction& transaction,
                        library::TrackStore::Writer& trackWriter,
                        library::FileManifestStore::Writer& manifestWriter,
                        library::DictionaryStore& dictionary,
                        library::TrackBuilder& builder,
                        AudioFingerprint const& fingerprint);

    void applyNewItem(ScanItem const& item,
                      lmdb::WriteTransaction& transaction,
                      library::TrackStore::Writer& trackWriter,
                      library::FileManifestStore::Writer& manifestWriter,
                      library::DictionaryStore& dictionary,
                      library::TrackBuilder& builder,
                      std::optional<AudioFingerprint> const& optFingerprint);

    std::optional<std::pair<std::unique_ptr<tag::TagFile>, library::TrackBuilder>> loadTrackBuilder(
      ScanItem const& item);

    std::optional<AudioFingerprint> cachedAudioFingerprint(ScanItem const& item) const noexcept;

    bool shouldFingerprintDuringApply(ScanItem const& item) const noexcept;

    bool requiresFingerprintForApply(ScanItem const& item) const noexcept;

    std::optional<AudioFingerprint> fingerprintAudioPayload(ScanItem const& item,
                                                            tag::TagFile const& tagFile,
                                                            std::size_t itemIndex,
                                                            std::stop_token stopToken);

    std::optional<std::pair<library::TrackBuilder::PreparedHot, library::TrackBuilder::PreparedCold>> prepareTrack(
      library::TrackBuilder const& builder,
      lmdb::WriteTransaction& transaction,
      library::DictionaryStore& dictionary,
      std::string const& uri);

    static library::FileManifestBuilder makeAvailableManifest(ScanItem const& item,
                                                              TrackId trackId,
                                                              std::optional<AudioFingerprint> const& optFingerprint);

    bool writeManifest(library::FileManifestStore::Writer& writer,
                       std::string const& uri,
                       library::FileManifestBuilder& builder);

    bool updateTrack(library::TrackStore::Writer& trackWriter,
                     TrackId trackId,
                     std::string const& uri,
                     library::TrackBuilder::PreparedHot const& hot,
                     library::TrackBuilder::PreparedCold const& cold);

    std::optional<TrackId> createTrack(library::TrackStore::Writer& trackWriter,
                                       std::string const& uri,
                                       library::TrackBuilder::PreparedHot const& hot,
                                       library::TrackBuilder::PreparedCold const& cold);

    library::MusicLibrary& _ml;
    ScanPlan _plan;
    ScanApplyOptions _options{};
    std::move_only_function<void(ScanApplyProgress const& progress)> _progressCallback;
    std::move_only_function<void(ScanFailure const& failure)> _itemFailureCallback;

    ScanApplyResult _result;
    bool _abortTransaction = false;
  };
} // namespace ao::rt
