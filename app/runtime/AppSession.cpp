// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "AppSession.h"
#include "EventBus.h"
#include "EventTypes.h"
#include "ISessionPersistence.h"
#include "LibraryMutationService.h"
#include "NotificationService.h"
#include "PlaybackService.h"
#include "ViewService.h"
#include "WorkspaceService.h"

#include <ao/library/MusicLibrary.h>
#include <ao/model/AllTrackIdsList.h>
#include <ao/model/SmartListEngine.h>

#include <memory>

namespace ao::app
{
  struct AppSession::Impl final
  {
    std::shared_ptr<IControlExecutor> executor;

    EventBus eventBus;

    ao::library::MusicLibrary musicLibrary;
    ao::model::AllTrackIdsList allTracksSource;
    ao::model::SmartListEngine smartListEngine;

    ViewService viewService;
    PlaybackService playbackService;
    LibraryMutationService mutationService;
    NotificationService notificationService;
    WorkspaceService workspaceService;

    Impl(std::shared_ptr<IControlExecutor> exec,
         std::filesystem::path libraryRoot,
         std::shared_ptr<ISessionPersistence> persistence)
      : executor{std::move(exec)}
      , musicLibrary{std::move(libraryRoot)}
      , allTracksSource{musicLibrary.tracks()}
      , smartListEngine{musicLibrary}
      , viewService{musicLibrary, smartListEngine, allTracksSource, eventBus}
      , playbackService{eventBus, *this->executor, viewService, musicLibrary}
      , mutationService{eventBus, *this->executor, musicLibrary}
      , notificationService{eventBus}
      , workspaceService{eventBus, viewService, playbackService, musicLibrary, std::move(persistence)}
    {
    }
  };

  AppSession::AppSession(AppSessionDependencies dependencies)
    : _impl{std::make_unique<Impl>(std::move(dependencies.executor),
                                   std::move(dependencies.libraryRoot),
                                   std::move(dependencies.persistence))}
  {
  }

  AppSession::~AppSession() = default;

  IControlExecutor& AppSession::executor() noexcept
  {
    return *_impl->executor;
  }

  EventBus& AppSession::events() noexcept
  {
    return _impl->eventBus;
  }

  PlaybackService& AppSession::playback() noexcept
  {
    return _impl->playbackService;
  }

  WorkspaceService& AppSession::workspace() noexcept
  {
    return _impl->workspaceService;
  }

  LibraryMutationService& AppSession::mutation() noexcept
  {
    return _impl->mutationService;
  }

  NotificationService& AppSession::notifications() noexcept
  {
    return _impl->notificationService;
  }

  ViewService& AppSession::views() noexcept
  {
    return _impl->viewService;
  }

  ao::library::MusicLibrary& AppSession::musicLibrary() noexcept
  {
    return _impl->musicLibrary;
  }

  ao::model::TrackIdList& AppSession::allTracks() noexcept
  {
    return _impl->allTracksSource;
  }

  ao::model::AllTrackIdsList& AppSession::allTrackIdsList() noexcept
  {
    return _impl->allTracksSource;
  }

  ao::model::SmartListEngine& AppSession::smartListEngine() noexcept
  {
    return _impl->smartListEngine;
  }

  void AppSession::reloadAllTracks()
  {
    auto txn = _impl->musicLibrary.readTransaction();
    _impl->allTracksSource.reloadFromStore(txn);
  }

  ao::TrackId AppSession::playSelectionInFocusedView()
  {
    auto const focus = _impl->workspaceService.layoutState();
    if (focus.activeViewId == ViewId{})
    {
      return ao::TrackId{};
    }
    return _impl->playbackService.playSelectionInView(focus.activeViewId);
  }

  void AppSession::addAudioProvider(std::unique_ptr<ao::audio::IBackendProvider> provider)
  {
    _impl->playbackService.addProvider(std::move(provider));
  }
}
