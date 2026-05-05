// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "AppSession.h"
#include "ObservableStore.h"

#include <ao/library/MusicLibrary.h>
#include <ao/model/AllTrackIdsList.h>
#include <ao/model/SmartListEngine.h>

#include <memory>

namespace ao::app
{
  struct AppSession::Impl final
  {
    std::shared_ptr<IControlExecutor> executor;

    CommandBus commandBus;
    EventBus eventBus;

    ao::library::MusicLibrary musicLibrary;
    ao::model::AllTrackIdsList allTracksSource;
    ao::model::SmartListEngine smartListEngine;
    PlaybackService playbackService;
    LibraryMutationService mutationService;

    ObservableStore<FocusState> focusStore;
    ViewRegistry viewRegistry;
    NotificationService notificationService;
    LibraryQueryService queryService;

    Impl(std::shared_ptr<IControlExecutor> exec, std::filesystem::path libraryRoot)
      : executor{std::move(exec)}
      , musicLibrary{std::move(libraryRoot)}
      , allTracksSource{musicLibrary.tracks()}
      , smartListEngine{musicLibrary}
      , playbackService{commandBus, eventBus, *this->executor}
      , mutationService{commandBus, eventBus, musicLibrary}
      , viewRegistry{musicLibrary, smartListEngine, allTracksSource, eventBus}
      , notificationService{eventBus}
      , queryService{viewRegistry}
    {
    }
  };

  AppSession::AppSession(AppSessionDependencies dependencies)
    : _impl{std::make_unique<Impl>(std::move(dependencies.executor), std::move(dependencies.libraryRoot))}
  {
  }

  AppSession::~AppSession() = default;

  IControlExecutor& AppSession::executor() noexcept
  {
    return *_impl->executor;
  }

  CommandBus& AppSession::commands() noexcept
  {
    return _impl->commandBus;
  }

  EventBus& AppSession::events() noexcept
  {
    return _impl->eventBus;
  }

  IReadOnlyStore<PlaybackState>& AppSession::playback() noexcept
  {
    return _impl->playbackService.state();
  }

  IReadOnlyStore<FocusState>& AppSession::focus() noexcept
  {
    return _impl->focusStore;
  }

  IReadOnlyStore<NotificationFeedState>& AppSession::notifications() noexcept
  {
    return _impl->notificationService.feed();
  }

  ViewRegistry& AppSession::views() noexcept
  {
    return _impl->viewRegistry;
  }

  LibraryQueryService& AppSession::queries() noexcept
  {
    return _impl->queryService;
  }

  NotificationService& AppSession::notificationService() noexcept
  {
    return _impl->notificationService;
  }
}
