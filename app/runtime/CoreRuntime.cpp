// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/async/Executor.h>
#include <ao/async/Runtime.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/CoreRuntime.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/completion/CompletionService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/source/TrackSourceCache.h>

#include <filesystem>
#include <memory>
#include <utility>

namespace ao::rt
{
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

    Impl(std::unique_ptr<async::Executor> execPtr, std::filesystem::path musicRoot, std::filesystem::path databasePath)
      : executorPtr{std::move(execPtr)}
      , asyncRuntime{*executorPtr}
      , musicRoot{std::move(musicRoot)}
      , databasePath{std::move(databasePath)}
      , musicLibrary{this->musicRoot, this->databasePath}
      , libraryChanges{}
      , libraryFacade{asyncRuntime, musicLibrary, libraryChanges}
      , completionService{musicLibrary, libraryChanges}
      , trackSourceCache{musicLibrary, libraryChanges}
      , notificationService{}
    {
    }
  };

  CoreRuntime::CoreRuntime(std::unique_ptr<async::Executor> executorPtr,
                           std::filesystem::path musicRoot,
                           std::filesystem::path databasePath)
    : _implPtr{std::make_unique<Impl>(std::move(executorPtr), std::move(musicRoot), std::move(databasePath))}
  {
  }

  CoreRuntime::~CoreRuntime() = default;

  library::MusicLibrary& CoreRuntime::musicLibrary() noexcept
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
