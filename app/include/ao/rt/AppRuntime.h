// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/library/LibraryDurability.h>

#include <cstdint>
#include <filesystem>
#include <functional>
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
  class Library;
  class LibraryCommands;
  class LibraryJobs;
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
    LibraryDurability musicLibraryDurability = LibraryDurability::Durable;
    /// Required owning store for workspace persistence and the default playback-session store.
    std::unique_ptr<ConfigStore> workspaceConfigStorePtr{};
    /// Optional borrowed override. When non-null, the store must outlive AppRuntime;
    /// null selects the owned workspace store.
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

  /// Borrowed interactive services. Library editing and maintenance are granted separately at composition.
  class AppRuntime final
  {
  public:
    AppRuntime(AppRuntime const&) = delete;
    AppRuntime& operator=(AppRuntime const&) = delete;
    AppRuntime(AppRuntime&&) = delete;
    AppRuntime& operator=(AppRuntime&&) = delete;

    Library const& library() const noexcept;
    LibraryJobs const& jobProgress() const noexcept;
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

    Result<> savePlaybackSession();
    Result<PlaybackSessionRestoreResult> restorePlaybackSession();
    Result<> discardRestorablePlaybackSession();
    /** Establishes playback save observation without loading a persisted session. */
    void startPlaybackSessionPersistence();
    /** Permanently disables playback-session writes without performing persistence I/O. */
    void sealPlaybackSessionPersistenceWrites();
    /** Removes persisted playback intent and permanently seals persistence for a library switch. */
    Result<> retirePlaybackSessionForLibrarySwitch();

    /// Start the first selected track present in the focused view's playback projection.
    Result<TrackId> playSelectionInFocusedView();
    Result<> jumpToAlbum(TrackId trackId);
    void addAudioProvider(std::unique_ptr<audio::BackendProvider> providerPtr);

    /// Updates ordering on the callback executor. Captured playback projections retain their prior order.
    void setTextOrderingPolicy(std::shared_ptr<TextOrderingPolicy const> policyPtr);

  private:
    friend class AppRuntimeOwner;
    struct Impl;
    explicit AppRuntime(Impl& impl) noexcept
      : _impl{impl}
    {
    }
    ~AppRuntime() = default;
    Impl& _impl;
  };

  /// The composition root owns this handle. Its service and capability borrows stay at fixed addresses across moves.
  class AppRuntimeOwner final
  {
  public:
    /// Session factories can distribute commands at composition without publishing this owner or its jobs.
    using BindLibraryCommands = std::function<void(LibraryCommands&)>;

    static Result<AppRuntimeOwner> open(AppRuntimeDependencies dependencies);
    ~AppRuntimeOwner();
    AppRuntimeOwner(AppRuntimeOwner const&) = delete;
    AppRuntimeOwner& operator=(AppRuntimeOwner const&) = delete;
    AppRuntimeOwner(AppRuntimeOwner&&) noexcept;
    AppRuntimeOwner& operator=(AppRuntimeOwner&&) = delete;

    // Keep the named owner (or its Result) alive for the duration of every borrow.
    AppRuntime& runtime() & noexcept;
    LibraryCommands& libraryCommands() & noexcept;
    LibraryJobs& libraryJobs() & noexcept;
    void shutdown() noexcept;

  private:
    explicit AppRuntimeOwner(std::unique_ptr<AppRuntime::Impl> implPtr);
    std::unique_ptr<AppRuntime::Impl> _implPtr;
  };
} // namespace ao::rt
