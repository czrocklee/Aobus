// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/CoreRuntime.h>

#include <ao/Error.h>
#include <ao/async/AsyncExceptionHandler.h>
#include <ao/async/Executor.h>
#include <ao/async/Runtime.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/completion/CompletionService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/source/TrackSourceCache.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <utility>

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
    std::unique_ptr<library::MusicLibrary> musicLibraryPtr;
    LibraryChanges libraryChanges;
    std::unique_ptr<Library> libraryFacadePtr;
    CompletionService completionService;
    TrackSourceCache trackSourceCache;
    NotificationService notificationService;
    bool stopped = false;

    Impl(std::unique_ptr<async::Executor> execPtr,
         std::filesystem::path musicRoot,
         std::filesystem::path databasePath,
         std::unique_ptr<library::MusicLibrary> libraryPtr,
         async::Sleeper* sleeper,
         async::AsyncExceptionHandler asyncExceptionHandler)
      : executorPtr{std::move(execPtr)}
      , asyncRuntime{*executorPtr, std::move(asyncExceptionHandler), sleeper}
      , musicRoot{std::move(musicRoot)}
      , databasePath{std::move(databasePath)}
      , musicLibraryPtr{std::move(libraryPtr)}
      , libraryChanges{*executorPtr, currentLibraryRevision(*musicLibraryPtr)}
      , completionService{*musicLibraryPtr, libraryChanges}
      , trackSourceCache{*musicLibraryPtr, libraryChanges}
      , notificationService{asyncRuntime}
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
      if (libraryFacadePtr)
      {
        libraryFacadePtr->beginClosing();
      }

      asyncRuntime.requestStop();
      asyncRuntime.join();
    }
  };

  Result<std::unique_ptr<CoreRuntime>> CoreRuntime::create(std::unique_ptr<async::Executor> executorPtr,
                                                           std::filesystem::path musicRoot,
                                                           std::filesystem::path databasePath,
                                                           std::size_t const musicLibraryMapSize,
                                                           async::Sleeper* const sleeper,
                                                           async::AsyncExceptionHandler asyncExceptionHandler)
  {
    auto runtimePtr = std::unique_ptr<CoreRuntime>{new CoreRuntime{}};
    auto result = runtimePtr->initialize(std::move(executorPtr),
                                         std::move(musicRoot),
                                         std::move(databasePath),
                                         musicLibraryMapSize,
                                         sleeper,
                                         std::move(asyncExceptionHandler));

    if (!result)
    {
      return std::unexpected{result.error()};
    }

    return runtimePtr;
  }

  Result<> CoreRuntime::initialize(std::unique_ptr<async::Executor> executorPtr,
                                   std::filesystem::path musicRoot,
                                   std::filesystem::path databasePath,
                                   std::size_t const musicLibraryMapSize,
                                   async::Sleeper* const sleeper,
                                   async::AsyncExceptionHandler asyncExceptionHandler)
  {
    if (executorPtr == nullptr)
    {
      return makeError(Error::Code::InvalidInput, "CoreRuntime requires an executor");
    }

    auto storageRes = library::MusicLibrary::open(
      musicRoot, databasePath, library::MusicLibrary::Options{.mapSize = musicLibraryMapSize});

    if (!storageRes)
    {
      return std::unexpected{storageRes.error()};
    }

    auto storagePtr = std::make_unique<library::MusicLibrary>(std::move(*storageRes));
    auto implPtr = std::make_unique<Impl>(std::move(executorPtr),
                                          std::move(musicRoot),
                                          std::move(databasePath),
                                          std::move(storagePtr),
                                          sleeper,
                                          std::move(asyncExceptionHandler));
    auto libraryRes = Library::create(implPtr->asyncRuntime, *implPtr->musicLibraryPtr, implPtr->libraryChanges);

    if (!libraryRes)
    {
      return std::unexpected{libraryRes.error()};
    }

    implPtr->libraryFacadePtr = std::move(*libraryRes);
    implPtr->trackSourceCache.reloadAllTracks();
    _implPtr = std::move(implPtr);
    return {};
  }

  CoreRuntime::CoreRuntime() = default;
  CoreRuntime::~CoreRuntime() = default;

  void CoreRuntime::shutdown() noexcept
  {
    if (_implPtr)
    {
      _implPtr->shutdown();
    }
  }

  library::MusicLibrary const& CoreRuntime::musicLibrary() const noexcept
  {
    return *_implPtr->musicLibraryPtr;
  }

  Library const& CoreRuntime::library() const noexcept
  {
    return *_implPtr->libraryFacadePtr;
  }

  Library& CoreRuntime::library() noexcept
  {
    return *_implPtr->libraryFacadePtr;
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

  async::Runtime& CoreRuntime::async() noexcept
  {
    return _implPtr->asyncRuntime;
  }
} // namespace ao::rt
