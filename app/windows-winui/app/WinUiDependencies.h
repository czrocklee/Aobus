// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "status/ActivityStatusControl.h"
#include "track/TrackDetailControl.h"

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>

namespace ao::rt
{
  class AppRuntime;
}

namespace ao::uimodel
{
  class PlaybackCommandSurface;
}

namespace ao::winui
{
  class CoverArtPresenter;
  class LibrarySession;
  class TrackListController;
  class WindowsCoverArtLoader;
  class WindowsThemeCoordinator;

  struct WindowsUiViewDependencies final
  {
    winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBox quickFilterInput{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Image inspectorCoverImage{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Grid inspectorCoverPlaceholder{nullptr};
    TrackDetailControlConfig trackDetail;
    ActivityStatusControlConfig activityStatus;
    winrt::Microsoft::UI::Xaml::Controls::Image nowPlayingCoverImage{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Grid nowPlayingCoverPlaceholder{nullptr};
  };

  /// Construction-scoped borrowed collaborators for WinUI controls.
  ///
  /// Consumers must retain only the narrow references they need. The playback
  /// and library runtimes are replaceable and this bundle must be requested
  /// again after the corresponding LibrarySession callback.
  struct WinUiDependencies final
  {
    LibrarySession& session;
    rt::AppRuntime& libraryRuntime;
    rt::AppRuntime& playbackRuntime;
    uimodel::PlaybackCommandSurface& playbackCommands;
    TrackListController& trackList;
    WindowsCoverArtLoader& coverArtLoader;
    CoverArtPresenter& inspectorCoverArt;
    CoverArtPresenter& nowPlayingCoverArt;
    WindowsThemeCoordinator& theme;
  };
} // namespace ao::winui
