// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "AppSession.h"
#include "ConfigStore.h"
#include "LibraryMutationService.h"
#include "NotificationService.h"
#include "PlaybackService.h"
#include "ViewService.h"
#include "WorkspaceService.h"

#include "ListSourceStore.h"

#include <ao/library/MusicLibrary.h>

#include <memory>

namespace ao::app
{
  struct AppSession::Impl final
  {
    std::shared_ptr<IControlExecutor> executor;

    ao::library::MusicLibrary musicLibrary;
    LibraryMutationService mutationService;
    ListSourceStore sources;

    ViewService viewService;
    PlaybackService playbackService;
    NotificationService notificationService;
    WorkspaceService workspaceService;

    Impl(std::shared_ptr<IControlExecutor> exec,
         std::filesystem::path libraryRoot,
         std::shared_ptr<ConfigStore> configStore)
      : executor{std::move(exec)}
      , musicLibrary{std::move(libraryRoot)}
      , mutationService{*this->executor, musicLibrary}
      , sources{musicLibrary, mutationService}
      , viewService{musicLibrary, sources}
      , playbackService{*this->executor, viewService, musicLibrary}
      , notificationService{}
      , workspaceService{viewService, playbackService, mutationService, musicLibrary, std::move(configStore)}
    {
      viewService.setWorkspaceService(workspaceService);
      viewService.setLibraryMutationService(mutationService);
    }
  };

  AppSession::AppSession(AppSessionDependencies dependencies)
    : _impl{std::make_unique<Impl>(std::move(dependencies.executor),
                                   std::move(dependencies.libraryRoot),
                                   std::move(dependencies.configStore))}
  {
  }

  AppSession::~AppSession() = default;

  IControlExecutor& AppSession::executor() noexcept
  {
    return *_impl->executor;
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

  ListSourceStore& AppSession::sources() noexcept
  {
    return _impl->sources;
  }

  void AppSession::reloadAllTracks()
  {
    _impl->sources.reloadAllTracks();
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
