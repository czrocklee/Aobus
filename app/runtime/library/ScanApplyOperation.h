// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "MediaTrack.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/library/AudioIdentity.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/media/file/File.h>
#include <ao/rt/library/ScanPlan.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace ao::library
{
  class MusicLibrary;
  class DictionaryStore;
  class LibraryWrite;
  class TrackWriter;
  class WritableMusicLibrary;
  struct AudioIdentity;
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
                       compat::MoveOnlyFunction<void(ScanApplyProgress const& progress)> progressCallback,
                       compat::MoveOnlyFunction<void(ScanFailure const& failure)> itemFailureCallback,
                       ScanApplyOptions options = {});

    ~ScanApplyOperation();

    ScanApplyOperation(ScanApplyOperation const&) = delete;
    ScanApplyOperation& operator=(ScanApplyOperation const&) = delete;
    ScanApplyOperation(ScanApplyOperation&&) = delete;
    ScanApplyOperation& operator=(ScanApplyOperation&&) = delete;

    // Offline composition: prepares media outside the write transaction, then
    // acquires and commits an isolated writable-library session.
    Result<ScanApplyResult> run(std::stop_token stopToken = {});
    // Runtime composition must call prepare before acquiring its coordinator
    // mutation. apply performs no filesystem reads or audio hashing.
    Result<ScanApplyResult> prepare(std::stop_token stopToken = {});
    Result<ScanApplyResult> revalidatePreparedFiles(std::stop_token stopToken = {});
    Result<ScanApplyResult> apply(library::LibraryWrite& write, std::stop_token stopToken = {});
    bool cancelled() const noexcept;
    bool readyForMutation() const noexcept;
    bool transactionShouldCommit() const noexcept;

  private:
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
    void admitItemsAgainstDatabase(library::TrackWriter const& trackWriter, std::stop_token stopToken);
    Result<std::filesystem::path> resolveItemPath(ScanItem const& item) const;
    void revalidatePreparedRegularFile(std::size_t itemIndex);
    void revalidateMissingPath(std::size_t itemIndex);
    void revalidateMovedFile(std::size_t itemIndex, std::stop_token stopToken);

    Result<> applyScanItem(std::size_t itemIndex,
                           PreparedScanItem const* preparedItem,
                           library::TrackWriter& trackWriter,
                           library::DictionaryStore const& dictionary);

    bool skipNonActionableItem(ScanItem const& item);

    /// Embedded covers are a scan fact: a refreshed item takes the cover set its
    /// file now carries, in place of the references it held.
    static void applyFileCoverArt(library::TrackBuilder& merged, library::TrackBuilder const& parsed);

    void reportProgress(ScanItem const& item, std::size_t itemIndex, ScanApplyProgressStage stage, double itemFraction);

    void reportFailure(std::string_view uri, std::string_view stage, std::string_view message);
    void skipStaleItem(std::size_t itemIndex) noexcept;

    void applyMissingItem(ScanItem const& item, library::TrackWriter& trackWriter);

    Result<> applyChangedItem(ScanItem const& item,
                              library::TrackWriter& trackWriter,
                              library::DictionaryStore const& dictionary,
                              library::TrackBuilder& builder,
                              library::AudioIdentity const& identity);

    bool applyMovedItem(ScanItem const& item,
                        library::TrackWriter& trackWriter,
                        library::DictionaryStore const& dictionary,
                        library::TrackBuilder& builder,
                        library::AudioIdentity const& identity);

    Result<> applyNewItem(ScanItem const& item,
                          library::TrackWriter& trackWriter,
                          library::TrackBuilder& builder,
                          std::optional<library::AudioIdentity> const& optIdentity);

    std::optional<MediaTrack> loadTrackBuilder(ScanItem const& item);

    std::optional<library::AudioIdentity> cachedAudioIdentity(ScanItem const& item) const noexcept;

    bool shouldFingerprintDuringPreparation(ScanItem const& item) const noexcept;

    bool isFingerprintRequiredForApply(ScanItem const& item) const noexcept;

    std::optional<library::AudioIdentity> fingerprintAudioPayload(ScanItem const& item,
                                                                  media::file::File const& file,
                                                                  std::size_t itemIndex,
                                                                  bool publishProgress,
                                                                  std::stop_token stopToken);

    bool validateTrack(library::TrackBuilder const& builder,
                       library::TrackWriter const& trackWriter,
                       std::string const& uri);

    static library::FileManifestBuilder makeAvailableManifest(ScanItem const& item,
                                                              std::optional<library::AudioIdentity> const& optIdentity);

    // Reports a rejection as an item failure and returns false. Use it only
    // where a false return is item-neutral, or where the caller has already
    // armed _abortTransaction for the complete item, as the Moved path does.
    bool writeManifest(library::TrackWriter& writer,
                       TrackId trackId,
                       std::string const& uri,
                       library::FileManifestBuilder& builder);

    library::MusicLibrary& _ml;
    ScanPlan _plan;
    ScanApplyOptions _options{};
    compat::MoveOnlyFunction<void(ScanApplyProgress const& progress)> _progressCallback;
    compat::MoveOnlyFunction<void(ScanFailure const& failure)> _itemFailureCallback;

    ScanApplyResult _result;
    std::vector<std::unique_ptr<PreparedScanItem>> _preparedItems;
    std::vector<bool> _skippedItems;
    State _state = State::Created;
    bool _cancelled = false;
    bool _abortTransaction = false;
    bool _manifestMutated = false;
  };
} // namespace ao::rt
