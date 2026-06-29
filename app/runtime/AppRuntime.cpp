// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/async/Runtime.h>
#include <ao/audio/IBackendProvider.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/CoreRuntime.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/source/ListSourceStore.h>

#include <memory>
#include <utility>

namespace ao::rt
{
  struct AppRuntime::Impl final
  {
    ViewService viewService;
    PlaybackService playbackService;
    WorkspaceService workspaceService;
    std::unique_ptr<ConfigStore> workspaceConfigStorePtr;

    Impl(AppRuntime& runtime, std::unique_ptr<ConfigStore> workspaceConfigPtr)
      : viewService{runtime.async().callbackExecutor(), runtime.musicLibrary(), runtime.sources()}
      , playbackService{runtime.async().callbackExecutor(), viewService, runtime.musicLibrary()}
      , workspaceService{viewService, playbackService, runtime.library().changes(), runtime.musicLibrary()}
      , workspaceConfigStorePtr{std::move(workspaceConfigPtr)}
    {
    }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    ~Impl() = default;
  };

  AppRuntime::AppRuntime(AppRuntimeDependencies dependencies)
    : CoreRuntime{std::move(dependencies.executorPtr),
                  std::move(dependencies.musicRoot),
                  std::move(dependencies.databasePath)}
    , _implPtr{std::make_unique<Impl>(*this, std::move(dependencies.workspaceConfigStorePtr))}
  {
  }

  AppRuntime::~AppRuntime() = default;

  PlaybackService& AppRuntime::playback() noexcept
  {
    return _implPtr->playbackService;
  }

  WorkspaceService& AppRuntime::workspace() noexcept
  {
    return _implPtr->workspaceService;
  }

  ViewService& AppRuntime::views() noexcept
  {
    return _implPtr->viewService;
  }

  ConfigStore& AppRuntime::configStore() noexcept
  {
    return *_implPtr->workspaceConfigStorePtr;
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
    auto const focus = _implPtr->workspaceService.layoutState();

    if (focus.activeViewId == rt::kInvalidViewId)
    {
      return kInvalidTrackId;
    }

    return _implPtr->playbackService.playSelectionInView(focus.activeViewId);
  }

  void AppRuntime::addAudioProvider(std::unique_ptr<audio::IBackendProvider> providerPtr)
  {
    _implPtr->playbackService.addProvider(std::move(providerPtr));
  }
} // namespace ao::rt
