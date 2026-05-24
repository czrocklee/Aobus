// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/Type.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/CoreRuntime.h>
#include <ao/rt/ListSourceStore.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/async/Runtime.h>

#include <memory>
#include <utility>

namespace ao::rt
{
  struct AppRuntime::Impl final
  {
    ViewService viewService;
    PlaybackService playbackService;
    WorkspaceService workspaceService;
    std::shared_ptr<ConfigStore> workspaceConfigStore;

    Impl(AppRuntime& runtime, std::shared_ptr<ConfigStore> workspaceConfig)
      : viewService{runtime.async().controlExecutor(), runtime.musicLibrary(), runtime.sources()}
      , playbackService{runtime.async().controlExecutor(), viewService, runtime.musicLibrary()}
      , workspaceService{viewService, playbackService, runtime.mutation(), runtime.musicLibrary()}
      , workspaceConfigStore{std::move(workspaceConfig)}
    {
    }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    ~Impl() = default;
  };

  AppRuntime::AppRuntime(AppRuntimeDependencies dependencies)
    : CoreRuntime{std::move(dependencies.executor),
                  std::move(dependencies.musicRoot),
                  std::move(dependencies.databasePath)}
    , _impl{std::make_unique<Impl>(*this, std::move(dependencies.workspaceConfigStore))}
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

  ViewService& AppRuntime::views() noexcept
  {
    return _impl->viewService;
  }

  ConfigStore& AppRuntime::configStore() noexcept
  {
    return *_impl->workspaceConfigStore;
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
