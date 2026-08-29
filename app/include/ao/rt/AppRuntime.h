// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>

#include <cstdint>
#include <filesystem>
#include <memory>

namespace ao::audio
{
  class BackendProvider;
}

namespace ao::async
{
  class Executor;
  class Runtime;
  class Sleeper;
}

namespace ao::rt
{
  class CompletionAliasPolicy;
  class CompletionService;
  class ConfigStore;
  class CoreRuntime;
  class Library;
  class NotificationService;
  class PlaybackService;
  class ResourceByteMemoryCache;
  class TextOrderingPolicy;
  class TrackSourceCache;
  class WorkspaceService;
  class ViewService;

  struct AppRuntimeDependencies
  {
    std::unique_ptr<async::Executor> executorPtr{};
    std::filesystem::path musicRoot{};
    std::filesystem::path databasePath{};
    /// Where derived caches live, resolved by the composition root because the
    /// runtime does not discover platform application directories. Empty is
    /// supported: the cover-read walk then has one tier instead of two, because a
    /// cache is an optimization and losing it is not a startup failure.
    std::filesystem::path cacheDirectory{};
    std::uint64_t musicLibraryPinnedMapBytes = 0;
    /// Required owning store for workspace persistence and the default playback-session store.
    std::unique_ptr<ConfigStore> workspaceConfigStorePtr{};
    ConfigStore* playbackSessionConfigStore = nullptr;
    async::Sleeper* sleeper = nullptr;
    /// Optional interactive text order selected and owned by the composition root.
    TextOrderingPolicy const* textOrderingPolicy = nullptr;
    /// Optional transient completion spelling policy owned by the composition root.
    CompletionAliasPolicy const* completionAliasPolicy = nullptr;
  };

  struct PlaybackSessionRestoreResult final
  {
    bool restored = false;
    TrackId trackId = kInvalidTrackId;
    ListId sourceListId = kInvalidListId;
  };

  class AppRuntime final
  {
  public:
    static Result<std::unique_ptr<AppRuntime>> create(AppRuntimeDependencies dependencies);
    ~AppRuntime();

    void shutdown() noexcept;

    AppRuntime(AppRuntime const&) = delete;
    AppRuntime& operator=(AppRuntime const&) = delete;
    AppRuntime(AppRuntime&&) = delete;
    AppRuntime& operator=(AppRuntime&&) = delete;

    Library const& library() const noexcept;
    Library& library() noexcept;
    async::Runtime& async() noexcept;
    TrackSourceCache& sources() noexcept;
    NotificationService& notifications() noexcept;
    CompletionService& completion() noexcept;
    TextOrderingPolicy const* textOrderingPolicy() const noexcept;
    std::filesystem::path const& musicRoot() const noexcept;
    ResourceByteMemoryCache& resourceBytes() noexcept;

    PlaybackService& playback() noexcept;
    WorkspaceService& workspace() noexcept;
    ViewService& views() noexcept;
    ConfigStore& workspaceConfigStore() noexcept;
    ConfigStore& playbackSessionConfigStore() noexcept;

    Result<> savePlaybackSession();
    Result<PlaybackSessionRestoreResult> restorePlaybackSession();
    Result<> discardRestorablePlaybackSession();
    /** Establishes playback save observation without loading a persisted session. */
    void startPlaybackSessionPersistence();
    /** Permanently disables playback-session writes without performing persistence I/O. */
    void sealPlaybackSessionPersistenceWrites();
    /** Removes persisted playback intent and permanently seals persistence for a library switch. */
    Result<> retirePlaybackSessionForLibrarySwitch();

    void reloadAllTracks();

    Result<TrackId> playSelectionInFocusedView();
    Result<> jumpToAlbum(TrackId trackId);
    void addAudioProvider(std::unique_ptr<audio::BackendProvider> providerPtr);

  private:
    struct Impl;
    AppRuntime(std::unique_ptr<CoreRuntime> corePtr,
               std::unique_ptr<ConfigStore> workspaceConfigStorePtr,
               ConfigStore* playbackSessionConfigStore);
    // Reverse member destruction retires every interactive borrower, including
    // ResourceByteMemoryCache, before the CoreRuntime they call into.
    std::unique_ptr<CoreRuntime> _corePtr;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
