// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/i18n/MessageCatalog.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ao::tui
{
  enum class TuiTextId : std::uint8_t
  {
    CommandPaletteTitle,
    CommandPaletteNoMatches,
    CommandPaletteFooter,
    QuickFilterTitle,
    QuickFilterFooter,
    WorkspaceList,
    WorkspaceView,
    OverlayTracks,
    OverlayLists,
    OverlayDetail,
    OverlayPipeline,
    OverlayOutput,
    OverlayViews,
    OverlayNotifications,
    OverlayHelp,
    HintLists,
    HintDetail,
    HintPipeline,
    HintOutput,
    HintViews,
    HintNotifications,
    HintHelp,
    StatusCommand,
    StatusLists,
    StatusView,
    StatusDetail,
    StatusClearFilter,
    StatusHelp,
    FilterLabel,
    CategoryLibrary,
    CategoryView,
    CategoryTrack,
    CategoryAudio,
    CategoryStatus,
    CategoryUi,
    CategoryPlayback,
    CategoryApp,
    DetailQuickFilter,
    DetailTrackView,
    DetailChooseList,
    DetailTrackDetail,
    DetailAudioPipeline,
    DetailOutputDevice,
    DetailChooseView,
    DetailNotificationCenter,
    DetailCloseOverlay,
    DetailHelp,
    DetailNowPlaying,
    DetailClearFilter,
    DetailReloadList,
    DetailPlay,
    DetailPause,
    DetailTogglePlayback,
    DetailStop,
    DetailQuit,
    OutputDevicesTitle,
    PlaybackNoActiveTrack,
    PlaybackNoOutputDeviceSelected,
    PlaybackNoOutputDevicesFound,
    PlaybackNoAudioPipeline,
    HelpQuickFilter,
    HelpChooseList,
    HelpTrackDetail,
    HelpAudioPipeline,
    HelpOutputDevice,
    HelpChooseView,
    HelpNotifications,
    HelpCurrentTrack,
    HelpSwitchPresentation,
    HelpPreviousNextGroup,
    HelpClearFilter,
    HelpReloadList,
    HelpPlayback,
    HelpQuit,
    HelpFooter,
    NotificationFooter,
    LibraryDetail,
    LibraryNoSections,
    LibraryNoSectionSelected,
    LibraryNoCurrentTrack,
    LibraryCurrentTrackNotInView,
    LibraryNoActiveTrackView,
    LibraryNoViewsAvailable,
    LibraryNoListsAvailable,
    LibraryFilterCleared,
    LibraryFilterApplied,
    LibraryNoTracksFound,
    LibraryNoListsFound,
    LibraryReady,
    Count,
  };

  /** TUI-local copy resolved once from the process catalog. */
  class TuiTextCatalog final
  {
  public:
    explicit TuiTextCatalog(i18n::MessageCatalog const& catalog);

    std::string_view text(TuiTextId id) const noexcept;
    std::string playbackVolume(std::int32_t percent) const;
    std::string librarySection(std::string_view name) const;
    std::string libraryRevealedTrack(std::string_view track) const;
    std::string libraryUnknownView(std::string_view id) const;
    std::string libraryView(std::string_view id) const;
    std::string libraryOpenedList(std::string_view list) const;
    std::string libraryReloadedTracks(std::size_t count) const;
    std::string libraryQuickFilterMatched(std::size_t count) const;
    std::string libraryExpressionFilterMatched(std::size_t count) const;

  private:
    i18n::MessageCatalog _catalog;
    std::array<std::string, static_cast<std::size_t>(TuiTextId::Count)> _text;
  };
} // namespace ao::tui
