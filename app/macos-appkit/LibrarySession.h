// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "LibraryEditorModel.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/OutputDeviceSelection.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/NotificationIds.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/TrackRow.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/rt/resource/ResourceBytes.h>
#include <ao/uimodel/library/presentation/TrackGroupHeadingPresentation.h>
#include <ao/uimodel/library/presentation/TrackPresentationPickerViewModel.h>
#include <ao/uimodel/library/track/TrackDisplayIndex.h>
#include <ao/uimodel/library/track/TrackFilterView.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>
#include <ao/uimodel/playback/now-playing/NowPlayingViewModel.h>
#include <ao/uimodel/playback/output/OutputDeviceViewModel.h>
#include <ao/uimodel/playback/output/VolumeViewModel.h>
#include <ao/uimodel/playback/seek/PlaybackPosition.h>
#include <ao/uimodel/playback/seek/PlaybackPositionInteraction.h>
#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>
#include <ao/uimodel/playback/transport/TransportViewModel.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ao::rt
{
  struct LibraryChangeSet;
}

namespace ao::appkit
{
  class LibraryBrowserModel;
  class MediaPlayerAdapter;
  struct PlaybackSeekTarget;

  enum class DesktopInvalidation : std::uint8_t
  {
    Library,
    Playback,
    Activity,
    Editor,
  };

  inline constexpr std::size_t kPlaybackCommandCapacity = 8;

  // Native views sample this main-thread state; runtime observers never enter AppKit.
  struct DesktopViewState final
  {
    std::uint64_t tableRevision = 0;
    std::uint64_t listRevision = 0;
    uimodel::NowPlayingViewState nowPlaying{};
    uimodel::OutputDeviceViewState output{};
    uimodel::VolumeViewState volume{};
    uimodel::PlaybackPositionViewState position{};
    rt::PlaybackPositionRevision positionRevision{};
    uimodel::AobusSoulViewState soul{};
    uimodel::TrackFilterViewState filter{};
    uimodel::TrackPresentationPickerState presentation{};
    std::optional<rt::TrackPresetId> optListPresentationId{};
    uimodel::ActivityStatusViewState activity{};
    std::array<uimodel::TransportViewState, kPlaybackCommandCapacity> transport{};
    rt::ResourceBytes playingCover{};
    rt::ResourceBytes selectedCover{};
    bool scanning = false;
  };

  class [[nodiscard]] LibrarySession final
  {
  public:
    using AudioProviderFactory = decltype(&audio::createPlatformBackendProviders);

    /// The optional command-binding hook runs once after successful initialization and is not retained.
    /// The command borrow expires when the session closes.
    static Result<std::unique_ptr<LibrarySession>> create(
      std::filesystem::path const& root,
      std::filesystem::path const& stateRoot,
      bool restorePlayback,
      i18n::MessageCatalog catalog,
      std::function<void(DesktopInvalidation)> onInvalidated,
      AudioProviderFactory createProviders = audio::createPlatformBackendProviders,
      rt::AppRuntimeOwner::BindLibraryCommands bindLibraryCommands = {});
    ~LibrarySession();
    LibrarySession(LibrarySession const&) = delete;
    LibrarySession& operator=(LibrarySession const&) = delete;
    LibrarySession(LibrarySession&&) = delete;
    LibrarySession& operator=(LibrarySession&&) = delete;

    DesktopViewState const& state() const noexcept;
    std::chrono::milliseconds playbackElapsed() const;
    rt::AppRuntime& runtime() const noexcept;
    LibraryEditorModel& editor() const noexcept;
    i18n::MessageCatalog const& catalog() const noexcept;
    MediaPlayerAdapter& mediaPlayer() const noexcept;
    LibraryBrowserModel& browser() noexcept;
    uimodel::TrackDisplayIndex const& displayIndex() const noexcept;
    rt::TrackRow const* rowAt(std::size_t displayIndex);
    std::vector<TrackId> selection() const;
    std::optional<std::size_t> displayIndexOf(TrackId trackId) const;
    void select(std::vector<TrackId> ids);
    void navigate(ListId listId) const;
    Result<> setPresentation(std::optional<rt::TrackPresetId> const& optPresetId) const;
    void filter(std::string const& text);
    void play(TrackId trackId);
    void execute(uimodel::PlaybackCommand command);
    void seek(uimodel::SeekSliderUpdate const& update, PlaybackSeekTarget const& target);
    void seekBy(rt::PlaybackOccurrenceId presentedOccurrence, std::chrono::milliseconds delta) const;
    void setVolume(float volume);
    void selectOutput(audio::OutputDeviceSelection const& selection);
    std::optional<std::chrono::steady_clock::duration> refreshActivity();
    void dismissActivity();
    void hideActivityNotification(rt::NotificationId id);
    void rescan();
    void checkpoint() const;
    void sealMediaPlayerAdmission() noexcept;
    bool canClose() const noexcept;
    void close() noexcept;

  private:
    friend class LibraryBrowserModel;

    LibrarySession(i18n::MessageCatalog catalog, std::function<void(DesktopInvalidation)> onInvalidated);
    Result<> initialize(std::filesystem::path const& root,
                        std::filesystem::path const& stateRoot,
                        bool restorePlayback,
                        AudioProviderFactory createProviders);
    void onLibraryChanged(rt::LibraryChangeSet const& changeSet);
    void bindProjection();
    void rebuildRows();
    void refreshSelection();
    void updateSelection(std::vector<TrackId> ids);
    void refreshSelectedCover();
    void reportPlaybackFailure(Error const& error) const;
    struct Storage;
    std::unique_ptr<Storage> _storagePtr;
  };
} // namespace ao::appkit
