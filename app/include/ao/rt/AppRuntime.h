// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "CoreRuntime.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>

#include <cstddef>
#include <filesystem>
#include <memory>

namespace ao::audio
{
  class BackendProvider;
}

namespace ao::async
{
  class Runtime;
}

namespace ao::rt
{
  class ConfigStore;
  class PlaybackQueueService;
  class PlaybackService;
  class WorkspaceService;
  class ViewService;

  struct AppRuntimeDependencies
  {
    std::unique_ptr<async::Executor> executorPtr{};
    std::filesystem::path musicRoot{};
    std::filesystem::path databasePath{};
    std::size_t musicLibraryMapSize = 0;
    std::unique_ptr<ConfigStore> workspaceConfigStorePtr{};
  };

  struct PlaybackSessionRestoreResult final
  {
    bool restored = false;
    TrackId trackId = kInvalidTrackId;
    ListId sourceListId = kInvalidListId;
  };

  class AppRuntime final : public CoreRuntime
  {
  public:
    explicit AppRuntime(AppRuntimeDependencies dependencies);
    ~AppRuntime() override;

    AppRuntime(AppRuntime const&) = delete;
    AppRuntime& operator=(AppRuntime const&) = delete;
    AppRuntime(AppRuntime&&) = delete;
    AppRuntime& operator=(AppRuntime&&) = delete;

    PlaybackService& playback() noexcept;
    PlaybackQueueService& playbackQueue() noexcept;
    WorkspaceService& workspace() noexcept;
    ViewService& views() noexcept;
    ConfigStore& configStore() noexcept;

    Result<> savePlaybackSession();
    Result<PlaybackSessionRestoreResult> restorePlaybackSession();

    void reloadAllTracks();

    TrackId playSelectionInFocusedView();
    void addAudioProvider(std::unique_ptr<audio::BackendProvider> providerPtr);

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
