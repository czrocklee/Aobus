// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "AppRuntime.h"

#include "ConfigStore.h"
#include "CorePrimitives.h"
#include "CoreRuntime.h"
#include "ListSourceStore.h"
#include "PlaybackService.h"
#include "SessionPersistenceService.h"
#include "ViewService.h"
#include "WorkspaceService.h"
#include "ao/Type.h"
#include "async/Runtime.h"

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
      : viewService{runtime.async().controlExecutor(), runtime.musicLibrary(), runtime.sources()}
      , playbackService{runtime.async().controlExecutor(), viewService, runtime.musicLibrary()}
      , workspaceService{viewService, playbackService, runtime.mutation(), runtime.musicLibrary()}
      , persistenceService{workspaceService, viewService, playbackService, runtime.musicLibrary(), *configStore}
    {
    }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    ~Impl() = default;
  };

  AppRuntime::AppRuntime(AppRuntimeDependencies dependencies)
    : CoreRuntime(std::move(dependencies.executor), dependencies.libraryRoot)
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

  async::Runtime& AppRuntime::async() noexcept
  {
    return CoreRuntime::async();
  }

  void AppRuntime::reloadAllTracks()
  {
    sources().reloadAllTracks();
  }

  TrackId AppRuntime::playSelectionInFocusedView()
  {
    auto const focus = _impl->workspaceService.layoutState();

    if (focus.activeViewId == rt::kInvalidViewId)
    {
      return kInvalidTrackId;
    }

    return _impl->playbackService.playSelectionInView(focus.activeViewId);
  }

  void AppRuntime::addAudioProvider(std::unique_ptr<audio::IBackendProvider> provider)
  {
    _impl->playbackService.addProvider(std::move(provider));
  }
} // namespace ao::rt
