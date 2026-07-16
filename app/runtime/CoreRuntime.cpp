// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/async/AsyncExceptionHandler.h>
#include <ao/async/Executor.h>
#include <ao/async/Runtime.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/CoreRuntime.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/completion/CompletionService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/source/TrackSourceCache.h>

#include <cstddef>
#include <cstdint>
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
    library::MusicLibrary musicLibrary;
    LibraryChanges libraryChanges;
    Library libraryFacade;
    CompletionService completionService;
    TrackSourceCache trackSourceCache;
    NotificationService notificationService;

    Impl(std::unique_ptr<async::Executor> execPtr,
         std::filesystem::path musicRoot,
         std::filesystem::path databasePath,
         std::size_t musicLibraryMapSize,
         async::Sleeper* sleeper,
         async::AsyncExceptionHandler asyncExceptionHandler)
      : executorPtr{std::move(execPtr)}
      , asyncRuntime{*executorPtr, std::move(asyncExceptionHandler), sleeper}
      , musicRoot{std::move(musicRoot)}
      , databasePath{std::move(databasePath)}
      , musicLibrary{this->musicRoot,
                     this->databasePath,
                     library::MusicLibrary::Options{.mapSize = musicLibraryMapSize}}
      , libraryChanges{*executorPtr, currentLibraryRevision(musicLibrary)}
      , libraryFacade{asyncRuntime, musicLibrary, libraryChanges}
      , completionService{musicLibrary, libraryChanges}
      , trackSourceCache{musicLibrary, libraryChanges}
      , notificationService{}
    {
    }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    ~Impl()
    {
      // Stop worker coroutines before library-backed members are destroyed.
      // Runtime is declared before them, so its destructor would otherwise run
      // after the LMDB environment and its consumers have already torn down.
      asyncRuntime.requestStop();
      asyncRuntime.join();
    }
  };

  CoreRuntime::CoreRuntime(std::unique_ptr<async::Executor> executorPtr,
                           std::filesystem::path musicRoot,
                           std::filesystem::path databasePath,
                           std::size_t musicLibraryMapSize,
                           async::Sleeper* sleeper,
                           async::AsyncExceptionHandler asyncExceptionHandler)
    : _implPtr{std::make_unique<Impl>(std::move(executorPtr),
                                      std::move(musicRoot),
                                      std::move(databasePath),
                                      musicLibraryMapSize,
                                      sleeper,
                                      std::move(asyncExceptionHandler))}
  {
  }

  CoreRuntime::~CoreRuntime() = default;

  library::MusicLibrary const& CoreRuntime::musicLibrary() const noexcept
  {
    return _implPtr->musicLibrary;
  }

  Library const& CoreRuntime::library() const noexcept
  {
    return _implPtr->libraryFacade;
  }

  Library& CoreRuntime::library() noexcept
  {
    return _implPtr->libraryFacade;
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
