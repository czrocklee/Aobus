// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/WindowsUiCoordinator.h"

#include "app/LibrarySession.h"
#include "image/CoverArtPresenter.h"
#include "status/ActivityStatusControl.h"
#include "theme/WindowsThemeCoordinator.h"
#include "track/TrackDetailControl.h"
#include "track/TrackListController.h"
#include "track/TrackQuickFilterControl.h"
#include <ao/rt/AppRuntime.h>
#include <ao/uimodel/playback/command/PlaybackCommandSurface.h>

#include <utility>

namespace ao::winui
{
  struct WindowsUiCoordinator::Impl final
  {
    Impl(LibrarySession& sessionValue, WindowsUiViewDependencies views, WindowsUiCoordinatorCallbacks callbacksValue)
      : session{sessionValue}
      , callbacks{std::move(callbacksValue)}
      , quickFilter{TrackQuickFilterControlConfig{
          .input = std::move(views.quickFilterInput),
          .onError =
            [this](std::string status)
          {
            if (callbacks.onStatus)
            {
              callbacks.onStatus(std::move(status));
            }
          },
        }}
      , inspectorCoverArt{std::move(views.inspectorCoverImage), std::move(views.inspectorCoverPlaceholder)}
      , trackDetail{std::move(views.trackDetail)}
      , activityStatus{std::move(views.activityStatus)}
      , nowPlayingCoverArt{std::move(views.nowPlayingCoverImage), std::move(views.nowPlayingCoverPlaceholder)}
      , theme{session.stateRoot() / "windows-theme.yaml"}
    {
      trackList.setOnChanged(
        [this]
        {
          if (callbacks.onTrackListChanged)
          {
            callbacks.onTrackListChanged();
          }
        });
      bindLibrary();
      bindPlayback();
      session.setCallbacks({.onLibraryChanging =
                              [this]
                            {
                              trackDetail.unbind();
                              quickFilter.unbind();
                              trackList.unbind();

                              if (callbacks.onLibraryChanging)
                              {
                                callbacks.onLibraryChanging();
                              }
                            },
                            .onLibraryChanged =
                              [this]
                            {
                              bindLibrary();

                              if (callbacks.onLibraryChanged)
                              {
                                callbacks.onLibraryChanged();
                              }
                            },
                            .onLibraryTaskRuntimeChanged = [this](std::shared_ptr<rt::AppRuntime> runtimePtr)
                            { activityStatus.bind(std::move(runtimePtr)); },
                            .onPlaybackChanging =
                              [this]
                            {
                              nowPlayingCoverArt.unbind();

                              if (callbacks.onPlaybackChanging)
                              {
                                callbacks.onPlaybackChanging();
                              }
                            },
                            .onPlaybackChanged =
                              [this]
                            {
                              bindPlayback();

                              if (callbacks.onPlaybackChanged)
                              {
                                callbacks.onPlaybackChanged();
                              }
                            },
                            .onStatus =
                              [this](std::string status)
                            {
                              if (callbacks.onStatus)
                              {
                                callbacks.onStatus(std::move(status));
                              }
                            },
                            .onFailure =
                              [this](Error const& error)
                            {
                              if (callbacks.onFailure)
                              {
                                callbacks.onFailure(error);
                              }
                            }});
    }

    void bindLibrary()
    {
      trackList.bind(session.libraryRuntimePtr(), session.columnLayouts());
      quickFilter.bind(session.libraryRuntimePtr());
      trackDetail.bind(dependencies());
      activityStatus.bind(session.libraryRuntimePtr());
    }

    void bindPlayback() { nowPlayingCoverArt.bind(session.playbackRuntimePtr()); }

    void retire()
    {
      if (!active)
      {
        return;
      }

      active = false;
      session.setCallbacks({});
      trackList.setOnChanged({});
      activityStatus.unbind();
      trackDetail.unbind();
      quickFilter.unbind();
      trackList.unbind();
      nowPlayingCoverArt.unbind();
      callbacks = {};
    }

    WinUiDependencies dependencies()
    {
      return WinUiDependencies{
        .session = session,
        .libraryRuntime = session.libraryRuntime(),
        .playbackRuntime = session.playbackRuntime(),
        .playbackCommands = session.playbackCommands(),
        .trackList = trackList,
        .inspectorCoverArt = inspectorCoverArt,
        .nowPlayingCoverArt = nowPlayingCoverArt,
        .theme = theme,
      };
    }

    LibrarySession& session;
    WindowsUiCoordinatorCallbacks callbacks;
    TrackListController trackList;
    TrackQuickFilterControl quickFilter;
    CoverArtPresenter inspectorCoverArt;
    TrackDetailControl trackDetail;
    ActivityStatusControl activityStatus;
    CoverArtPresenter nowPlayingCoverArt;
    WindowsThemeCoordinator theme;
    bool active = true;
  };

  WindowsUiCoordinator::WindowsUiCoordinator(LibrarySession& session,
                                             WindowsUiViewDependencies views,
                                             WindowsUiCoordinatorCallbacks callbacks)
    : _implPtr{std::make_unique<Impl>(session, std::move(views), std::move(callbacks))}
  {
  }

  WindowsUiCoordinator::~WindowsUiCoordinator()
  {
    retire();
  }

  WinUiDependencies WindowsUiCoordinator::uiDependencies() const
  {
    return _implPtr->dependencies();
  }

  void WindowsUiCoordinator::retire()
  {
    _implPtr->retire();
  }
} // namespace ao::winui
