// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/WindowsUiCoordinator.h"

#include "app/LibrarySession.h"
#include "app/WinUiDependencies.h"
#include "image/CoverArtPresenter.h"
#include "status/ActivityStatusControl.h"
#include "theme/WindowsThemeCoordinator.h"
#include "track/TrackDetailControl.h"
#include "track/TrackListController.h"
#include "track/TrackQuickFilterControl.h"
#include <ao/Error.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/resource/ResourceByteLoader.h>
#include <ao/uimodel/presentation/CoverArtPlaceholder.h>

#include <memory>
#include <string>
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
      , theme{session.stateRoot() / "windows-theme.yaml"}
      , inspectorCoverArt{std::move(views.inspectorCoverImage),
                          std::move(views.inspectorCoverPlaceholder),
                          resourceBytes,
                          theme,
                          uimodel::defaultCoverArtPlaceholderStyle(uimodel::CoverArtPlaceholderSlot::Inspector)}
      , trackDetail{std::move(views.trackDetail)}
      , activityStatus{std::move(views.activityStatus)}
      , nowPlayingCoverArt{std::move(views.nowPlayingCoverImage),
                           std::move(views.nowPlayingCoverPlaceholder),
                           resourceBytes,
                           theme,
                           uimodel::defaultCoverArtPlaceholderStyle(uimodel::CoverArtPlaceholderSlot::NowPlaying)}
    {
      trackList.setOnChanged(
        [this]
        {
          if (callbacks.onTrackListChanged)
          {
            callbacks.onTrackListChanged();
          }
        });
      bindRuntime();
      session.setCallbacks({.onRuntimeChanging =
                              [this] noexcept
                            {
                              callbacks.onRuntimeChanging();
                              trackDetail.unbind();
                              activityStatus.unbind();
                              quickFilter.unbind();
                              trackList.unbind();
                              nowPlayingCoverArt.unbind();
                              resourceBytes.unbind();
                            },
                            .onRuntimeChanged =
                              [this] noexcept
                            {
                              bindRuntime();
                              callbacks.onRuntimeChanged();
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

    void bindRuntime()
    {
      resourceBytes.bind(session.runtimePtr());
      trackList.bind(session.runtimePtr(), session.columnLayouts());
      quickFilter.bind(session.runtimePtr());
      trackDetail.bind(dependencies());
      activityStatus.bind(session.runtimePtr());
      nowPlayingCoverArt.bind(session.runtime().async());
    }

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
      resourceBytes.unbind();
      callbacks = {};
    }

    WinUiDependencies dependencies()
    {
      return WinUiDependencies{
        .session = session,
        .runtime = session.runtime(),
        .playbackCommands = session.playbackCommands(),
        .trackList = trackList,
        .resourceBytes = resourceBytes,
        .inspectorCoverArt = inspectorCoverArt,
        .nowPlayingCoverArt = nowPlayingCoverArt,
        .theme = theme,
      };
    }

    LibrarySession& session;
    WindowsUiCoordinatorCallbacks callbacks;
    TrackListController trackList;
    TrackQuickFilterControl quickFilter;
    WindowsThemeCoordinator theme;
    rt::ResourceByteLoader resourceBytes;
    CoverArtPresenter inspectorCoverArt;
    TrackDetailControl trackDetail;
    ActivityStatusControl activityStatus;
    CoverArtPresenter nowPlayingCoverArt;
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
