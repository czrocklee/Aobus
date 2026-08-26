// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/UiCoordinator.h"

#include "app/LibrarySession.h"
#include "theme/ThemeCoordinator.h"
#include "track/TrackListController.h"
#include <ao/Error.h>
#include <ao/async/Subscription.h>
// ResourceByteLoader::bind converts AppRuntime to its CoreRuntime base, which requires a complete derived type.
#include <ao/rt/AppRuntime.h> // NOLINT(misc-include-cleaner)
#include <ao/rt/playback/PlaybackEvents.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/resource/ResourceByteLoader.h>

#include <memory>
#include <string>
#include <utility>

namespace ao::winui
{
  struct UiCoordinator::Impl final
  {
    Impl(LibrarySession& sessionValue, UiCoordinatorCallbacks callbacksValue)
      : session{sessionValue}
      , callbacks{std::move(callbacksValue)}
      , trackList{sessionValue.textCatalog()}
      , theme{session.stateRoot() / "windows-theme.yaml"}
    {
      bindRuntime();
      session.setCallbacks({.onStatus =
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
      resourceBytes.bind(session.runtime());
      trackList.bind(session.runtime(), session.columnLayouts());
      revealTrackSub = session.runtime().playback().events().onRevealTrackRequested(
        [this](rt::PlaybackRevealTrackRequest const& request)
        {
          auto const revealedRes =
            trackList.revealTrack(request.trackId, request.preferredViewId, request.preferredListId);

          if (!revealedRes && callbacks.onFailure)
          {
            callbacks.onFailure(revealedRes.error());
          }
        });
    }

    void retire() noexcept
    {
      if (!active)
      {
        return;
      }

      // Disable callback publication before releasing any borrowed model. The
      // individual unbinds then only have to finish cancellation and native
      // surface cleanup; none may re-enter the window.
      active = false;
      callbacks = {};
      session.setCallbacks({});
      revealTrackSub.reset();
      trackList.unbind();
      resourceBytes.unbind();
    }

    LibrarySession& session;
    UiCoordinatorCallbacks callbacks;
    TrackListController trackList;
    ThemeCoordinator theme;
    rt::ResourceByteLoader resourceBytes;
    async::Subscription revealTrackSub;
    bool active = true;
  };

  UiCoordinator::UiCoordinator(LibrarySession& session, UiCoordinatorCallbacks callbacks)
    : _implPtr{std::make_unique<Impl>(session, std::move(callbacks))}
  {
  }

  UiCoordinator::~UiCoordinator()
  {
    retire();
  }

  TrackListController& UiCoordinator::trackList() const noexcept
  {
    return _implPtr->trackList;
  }

  ThemeCoordinator& UiCoordinator::theme() const noexcept
  {
    return _implPtr->theme;
  }

  rt::ResourceByteLoader& UiCoordinator::resourceBytes() const noexcept
  {
    return _implPtr->resourceBytes;
  }

  void UiCoordinator::retire() noexcept
  {
    _implPtr->retire();
  }
} // namespace ao::winui
