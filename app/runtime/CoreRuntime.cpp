// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/CoreRuntime.h>

#include "resource/ResourceByteReader.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Executor.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/completion/CompletionService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/source/TrackSourceCache.h>
#include <ao/utility/Path.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <utility>
#include <vector>

namespace ao::rt
{
  namespace
  {
    std::uint64_t currentLibraryRevision(library::MusicLibrary const& library)
    {
      auto transaction = library.readTransaction();
      return library.libraryRevision(transaction);
    }
  } // namespace

  struct CoreRuntime::Impl final
  {
    std::unique_ptr<async::Executor> executorPtr;
    async::Runtime asyncRuntime;
    std::filesystem::path musicRoot;
    std::filesystem::path databasePath;
    library::MusicLibrary musicLibrary;
    LibraryChanges libraryChanges;
    std::optional<Library> optLibrary;
    CompletionService completionService;
    TrackSourceCache trackSourceCache;
    NotificationService notificationService;
    ResourceByteReader resourceByteReader;
    TextOrderingPolicy const* textOrderingPolicy = nullptr;
    bool stopped = false;

    Impl(std::unique_ptr<async::Executor> execPtr,
         std::filesystem::path musicRoot,
         std::filesystem::path databasePath,
         std::filesystem::path cacheDirectory,
         library::MusicLibrary&& library,
         async::Sleeper* sleeper,
         TextOrderingPolicy const* orderingPolicy,
         CompletionAliasPolicy const* aliasPolicy)
      : executorPtr{std::move(execPtr)}
      , asyncRuntime{*executorPtr, sleeper}
      , musicRoot{std::move(musicRoot)}
      , databasePath{std::move(databasePath)}
      , musicLibrary{std::move(library)}
      , libraryChanges{*executorPtr,
                       currentLibraryRevision(musicLibrary),
                       utility::pathToUtf8(musicLibrary.databasePath())}
      , completionService{musicLibrary, libraryChanges, orderingPolicy, aliasPolicy}
      , trackSourceCache{musicLibrary, libraryChanges}
      , notificationService{asyncRuntime}
      , resourceByteReader{asyncRuntime, musicLibrary, cacheDirectory}
      , textOrderingPolicy{orderingPolicy}
    {
    }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    ~Impl() { shutdown(); }

    void shutdown() noexcept
    {
      if (stopped)
      {
        return;
      }

      stopped = true;
      // Seal mutation/publication admission before callback resumption closes,
      // then stop worker coroutines before library-backed members are destroyed.
      // Runtime is declared before them, so its destructor would otherwise run
      // after the LMDB environment and its consumers have already torn down.
      if (optLibrary)
      {
        optLibrary->beginClosing();
      }

      asyncRuntime.requestStop();
      asyncRuntime.join();
    }
  };

  Result<CoreRuntime> CoreRuntime::create(std::unique_ptr<async::Executor> executorPtr,
                                          std::filesystem::path musicRoot,
                                          std::filesystem::path databasePath,
                                          std::filesystem::path cacheDirectory,
                                          std::uint64_t const musicLibraryPinnedMapBytes,
                                          async::Sleeper* const sleeper,
                                          TextOrderingPolicy const* const textOrderingPolicy,
                                          CompletionAliasPolicy const* const completionAliasPolicy)
  {
    if (executorPtr == nullptr)
    {
      return makeError(Error::Code::InvalidInput, "CoreRuntime requires an executor");
    }

    auto storageRes = library::MusicLibrary::open(
      musicRoot, databasePath, library::MusicLibrary::Options{.pinnedMapBytes = musicLibraryPinnedMapBytes});

    if (!storageRes)
    {
      return std::unexpected{storageRes.error()};
    }

    auto implPtr = std::make_unique<Impl>(std::move(executorPtr),
                                          std::move(musicRoot),
                                          std::move(databasePath),
                                          std::move(cacheDirectory),
                                          std::move(*storageRes),
                                          sleeper,
                                          textOrderingPolicy,
                                          completionAliasPolicy);
    auto preparedLibraryRes = Library::prepare(implPtr->musicLibrary);

    if (!preparedLibraryRes)
    {
      return std::unexpected{preparedLibraryRes.error()};
    }

    implPtr->optLibrary.emplace(implPtr->asyncRuntime, std::move(*preparedLibraryRes), implPtr->libraryChanges);
    implPtr->trackSourceCache.reloadAllTracks();
    return CoreRuntime{std::move(implPtr)};
  }

  CoreRuntime::CoreRuntime(std::unique_ptr<Impl> implPtr)
    : _implPtr{std::move(implPtr)}
  {
  }
  CoreRuntime::~CoreRuntime() = default;
  CoreRuntime::CoreRuntime(CoreRuntime&& other) noexcept = default;

  void CoreRuntime::shutdown() noexcept
  {
    if (_implPtr)
    {
      _implPtr->shutdown();
    }
  }

  library::MusicLibrary const& CoreRuntime::musicLibrary() const noexcept
  {
    return _implPtr->musicLibrary;
  }

  Library const& CoreRuntime::library() const noexcept
  {
    AO_INVARIANT(_implPtr->optLibrary, "Core runtime library is not initialized");
    return _implPtr->optLibrary.value();
  }

  Library& CoreRuntime::library() noexcept
  {
    AO_INVARIANT(_implPtr->optLibrary, "Core runtime library is not initialized");
    return _implPtr->optLibrary.value();
  }

  std::filesystem::path const& CoreRuntime::musicRoot() const noexcept
  {
    return _implPtr->musicRoot;
  }

  std::filesystem::path const& CoreRuntime::databasePath() const noexcept
  {
    return _implPtr->databasePath;
  }

  CompletionService& CoreRuntime::completion() noexcept
  {
    return _implPtr->completionService;
  }

  TrackSourceCache& CoreRuntime::sources() noexcept
  {
    return _implPtr->trackSourceCache;
  }

  NotificationService& CoreRuntime::notifications() noexcept
  {
    return _implPtr->notificationService;
  }

  TextOrderingPolicy const* CoreRuntime::textOrderingPolicy() const noexcept
  {
    return _implPtr->textOrderingPolicy;
  }

  async::Runtime& CoreRuntime::async() noexcept
  {
    return _implPtr->asyncRuntime;
  }

  async::Task<Result<std::optional<std::vector<std::byte>>>> CoreRuntime::readResourceBytesForExportAsync(
    ResourceId const resourceId,
    std::stop_token const stopToken)
  {
    return _implPtr->resourceByteReader.readForExportAsync(resourceId, stopToken);
  }

  async::Task<Result<std::optional<std::vector<std::byte>>>> CoreRuntime::readInteractiveResourceBytesAsync(
    ResourceId const resourceId,
    std::stop_token const stopToken)
  {
    return _implPtr->resourceByteReader.readInteractiveAsync(resourceId, stopToken);
  }
} // namespace ao::rt
