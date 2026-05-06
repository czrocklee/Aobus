// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "AppSession.h"
#include "CommandTypes.h"
#include "EventTypes.h"
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
      , playbackService{commandBus, eventBus, *this->executor, viewRegistry, musicLibrary}
      , mutationService{commandBus, eventBus, *this->executor, musicLibrary}
      , viewRegistry{musicLibrary, smartListEngine, allTracksSource, eventBus}
      , notificationService{commandBus, eventBus}
      , queryService{viewRegistry, eventBus, musicLibrary}
    {
      viewRegistry.registerCommandHandlers(commandBus);

      commandBus.registerHandler<SetFocusedView>(
        [this](SetFocusedView const& cmd) -> ao::Result<void>
        {
          focusStore.update(FocusState{
            .focusedView = cmd.viewId,
            .revision = focusStore.snapshot().revision + 1,
          });
          eventBus.publish(FocusedViewChanged{.viewId = cmd.viewId});
          return {};
        });

      commandBus.registerHandler<PlaySelectionInFocusedView>(
        [this](PlaySelectionInFocusedView const&) -> ao::Result<ao::TrackId>
        {
          auto const focus = focusStore.snapshot();
          if (focus.focusedView == ViewId{})
          {
            return ao::TrackId{};
          }
          return commandBus.execute<PlaySelectionInView>(PlaySelectionInView{.viewId = focus.focusedView});
        });
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

  void AppSession::addAudioProvider(std::unique_ptr<ao::audio::IBackendProvider> provider)
  {
    _impl->playbackService.addProvider(std::move(provider));
  }
}
