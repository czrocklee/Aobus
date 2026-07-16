// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "CoreRuntime.h"
#include "Subscription.h"
#include "WorkspaceSnapshot.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/AsyncExceptionHandler.h>

#include <cstddef>
#include <filesystem>
#include <functional>
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
  class PlaybackSequenceService;
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
    ConfigStore* playbackSessionConfigStore = nullptr;
    async::Sleeper* sleeper = nullptr;
    async::AsyncExceptionHandler asyncExceptionHandler{};
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
    PlaybackSequenceService& playbackSequence() noexcept;
    WorkspaceService& workspace() noexcept;
    ViewService& views() noexcept;
    ConfigStore& workspaceConfigStore() noexcept;
    ConfigStore& playbackSessionConfigStore() noexcept;

    Result<> savePlaybackSession();
    Result<PlaybackSessionRestoreResult> restorePlaybackSession();
    Result<> discardRestorablePlaybackSession();
    Subscription onPlaybackSessionDirty(std::move_only_function<void()> handler);

    void reloadAllTracks();

    Result<TrackId> playSelectionInFocusedView();
    Result<WorkspaceCommitReceipt> jumpToAlbum(TrackId trackId);
    void addAudioProvider(std::unique_ptr<audio::BackendProvider> providerPtr);

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
