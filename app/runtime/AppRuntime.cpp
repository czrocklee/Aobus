// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "AppRuntime.h"
#include "ConfigStore.h"
#include "CoreRuntime.h"
#include "PlaybackService.h"
#include "SessionPersistenceService.h"
#include "ViewService.h"
#include "WorkspaceService.h"

#include "ListSourceStore.h"

#include <ao/Type.h>

#include <memory>
#include <utility>

namespace ao::rt
{
  struct AppRuntime::Impl final
  {
    ViewService viewService;
    PlaybackService playbackService;
    WorkspaceService workspaceService;
    SessionPersistenceService persistenceService;

    Impl(AppRuntime& runtime, std::shared_ptr<ConfigStore> configStore)
      : viewService{runtime.executor(), runtime.musicLibrary(), runtime.sources()}
      , playbackService{runtime.executor(), viewService, runtime.musicLibrary()}
      , workspaceService{viewService, playbackService, runtime.mutation(), runtime.musicLibrary()}
      , persistenceService{workspaceService, viewService, playbackService, runtime.musicLibrary(), *configStore}
    {
    }
  };

  AppRuntime::AppRuntime(AppRuntimeDependencies dependencies)
    : CoreRuntime(dependencies.executor, dependencies.libraryRoot)
    , _impl{std::make_unique<Impl>(*this, std::move(dependencies.configStore))}
  {
  }

  AppRuntime::~AppRuntime() = default;

  PlaybackService& AppRuntime::playback() noexcept
  {
    return _impl->playbackService;
  }

  WorkspaceService& AppRuntime::workspace() noexcept
  {
    return _impl->workspaceService;
  }

  SessionPersistenceService& AppRuntime::persistence() noexcept
  {
    return _impl->persistenceService;
  }

  ViewService& AppRuntime::views() noexcept
  {
    return _impl->viewService;
  }

  void AppRuntime::reloadAllTracks()
  {
    sources().reloadAllTracks();
  }

  TrackId AppRuntime::playSelectionInFocusedView()
  {
    auto const focus = _impl->workspaceService.layoutState();

    if (focus.activeViewId == ViewId{})
    {
      return TrackId{};
    }

    return _impl->playbackService.playSelectionInView(focus.activeViewId);
  }

  void AppRuntime::addAudioProvider(std::unique_ptr<audio::IBackendProvider> provider)
  {
    _impl->playbackService.addProvider(std::move(provider));
  }
} // namespace ao::rt
