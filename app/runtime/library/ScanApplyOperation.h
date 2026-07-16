// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "MediaTrack.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/library/WriteTransaction.h>
#include <ao/media/file/File.h>
#include <ao/rt/library/ScanPlan.h>
#include <ao/utility/Hash128.h>

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
#include <vector>

namespace ao::library
{
  class MusicLibrary;
  class DictionaryStore;
  class WritableMusicLibrary;
}

namespace ao::rt
{
  /**
   * ScanApplyOperation - runs one ScanPlan application against the MusicLibrary database.
   *
   * This class focuses exclusively on reconciling the filesystem scan results
   * with the database state.
   */
  class ScanApplyOperation final
  {
  public:
    ScanApplyOperation(library::MusicLibrary& ml,
                       ScanPlan plan,
                       std::move_only_function<void(ScanApplyProgress const& progress)> progressCallback,
                       std::move_only_function<void(ScanFailure const& failure)> itemFailureCallback,
                       ScanApplyOptions options = {});

    ~ScanApplyOperation();

    ScanApplyOperation(ScanApplyOperation const&) = delete;
    ScanApplyOperation& operator=(ScanApplyOperation const&) = delete;
    ScanApplyOperation(ScanApplyOperation&&) = delete;
    ScanApplyOperation& operator=(ScanApplyOperation&&) = delete;

    // Offline composition: prepares media outside the write transaction, then
    // acquires and commits an isolated writable-library session.
    Result<ScanApplyResult> run(std::stop_token stopToken = {});
    Result<ScanApplyResult> runOffline(library::WritableMusicLibrary& writableLibrary, std::stop_token stopToken = {});
    // Runtime composition must call prepare before acquiring its coordinator
    // mutation. apply performs no filesystem reads or audio hashing.
    Result<ScanApplyResult> prepare(std::stop_token stopToken = {});
    Result<ScanApplyResult> revalidatePreparedFiles(std::stop_token stopToken = {});
    Result<ScanApplyResult> apply(library::WriteTransaction& transaction, std::stop_token stopToken = {});
    bool cancelled() const noexcept;
    bool readyForMutation() const noexcept;
    bool transactionShouldCommit() const noexcept;

  private:
    struct AudioFingerprint final
    {
      utility::Hash128 signature;
      std::uint64_t payloadLength = 0;
    };

    struct PreparedScanItem;

    enum class State : std::uint8_t
    {
      Created,
      Prepared,
      Revalidated,
      Applied,
      Terminal,
    };

    Result<> validatePlan() const;
    Result<std::filesystem::path> resolveItemPath(ScanItem const& item) const;

    void applyScanItem(std::size_t itemIndex,
                       PreparedScanItem const* preparedItem,
                       library::WriteTransaction& transaction,
                       library::TrackStore::Writer& trackWriter,
                       library::FileManifestStore::Writer& manifestWriter,
                       library::DictionaryStore const& dictionary);

    bool skipNonActionableItem(ScanItem const& item);

    void reportProgress(ScanItem const& item, std::size_t itemIndex, ScanApplyProgressStage stage, double itemFraction);

    void reportFailure(std::string_view uri, std::string_view stage, std::string_view message);

    void applyMissingItem(ScanItem const& item,
                          library::WriteTransaction& transaction,
                          library::FileManifestStore::Writer& manifestWriter);

    bool tryApplyChangedItem(ScanItem const& item,
                             library::WriteTransaction& transaction,
                             library::TrackStore::Writer& trackWriter,
                             library::FileManifestStore::Writer& manifestWriter,
                             library::DictionaryStore const& dictionary,
                             library::TrackBuilder& builder,
                             AudioFingerprint const& fingerprint);

    bool applyMovedItem(ScanItem const& item,
                        library::WriteTransaction& transaction,
                        library::TrackStore::Writer& trackWriter,
                        library::FileManifestStore::Writer& manifestWriter,
                        library::DictionaryStore const& dictionary,
                        library::TrackBuilder& builder,
                        AudioFingerprint const& fingerprint);

    void applyNewItem(ScanItem const& item,
                      library::WriteTransaction& transaction,
                      library::TrackStore::Writer& trackWriter,
                      library::FileManifestStore::Writer& manifestWriter,
                      library::TrackBuilder& builder,
                      std::optional<AudioFingerprint> const& optFingerprint);

    std::optional<MediaTrack> loadTrackBuilder(ScanItem const& item);

    std::optional<AudioFingerprint> cachedAudioFingerprint(ScanItem const& item) const noexcept;

    bool shouldFingerprintDuringPreparation(ScanItem const& item) const noexcept;

    bool isFingerprintRequiredForApply(ScanItem const& item) const noexcept;

    std::optional<AudioFingerprint> fingerprintAudioPayload(ScanItem const& item,
                                                            media::file::File const& file,
                                                            std::size_t itemIndex,
                                                            bool publishProgress,
                                                            std::stop_token stopToken);

    std::optional<std::pair<library::TrackBuilder::PreparedHot, library::TrackBuilder::PreparedCold>>
    prepareTrack(library::TrackBuilder const& builder, library::WriteTransaction& transaction, std::string const& uri);

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
    std::vector<std::unique_ptr<PreparedScanItem>> _preparedItems;
    State _state = State::Created;
    bool _cancelled = false;
    bool _abortTransaction = false;
  };
} // namespace ao::rt
