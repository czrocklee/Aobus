// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/async/Runtime.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/CompletionService.h>
#include <ao/rt/CoreRuntime.h>
#include <ao/rt/LibraryMutationService.h>
#include <ao/rt/ListSourceStore.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/TrackCommandService.h>

#include <filesystem>
#include <memory>
#include <utility>

namespace ao::rt
{
  struct CoreRuntime::Impl final
  {
    std::unique_ptr<async::IExecutor> executorPtr;
    async::Runtime asyncRuntime;
    std::filesystem::path musicRoot;
    std::filesystem::path databasePath;
    library::MusicLibrary musicLibrary;
    LibraryMutationService mutationService;
    CompletionService completionService;
    TrackCommandService trackCommandService;
    ListSourceStore listSourceStore;
    NotificationService notificationService;

    Impl(std::unique_ptr<async::IExecutor> execPtr, std::filesystem::path musicRoot, std::filesystem::path databasePath)
      : executorPtr{std::move(execPtr)}
      , asyncRuntime{*executorPtr}
      , musicRoot{std::move(musicRoot)}
      , databasePath{std::move(databasePath)}
      , musicLibrary{this->musicRoot, this->databasePath}
      , mutationService{asyncRuntime, musicLibrary}
      , completionService{musicLibrary, mutationService}
      , trackCommandService{musicLibrary, mutationService}
      , listSourceStore{musicLibrary, mutationService}
      , notificationService{}
    {
    }
  };

  CoreRuntime::CoreRuntime(std::unique_ptr<async::IExecutor> executorPtr,
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

  std::filesystem::path const& CoreRuntime::musicRoot() const noexcept
  {
    return _implPtr->musicRoot;
  }

  std::filesystem::path const& CoreRuntime::databasePath() const noexcept
  {
    return _implPtr->databasePath;
  }

  LibraryMutationService& CoreRuntime::mutation() noexcept
  {
    return _implPtr->mutationService;
  }

  CompletionService& CoreRuntime::completion() noexcept
  {
    return _implPtr->completionService;
  }

  TrackCommandService& CoreRuntime::trackCommands() noexcept
  {
    return _implPtr->trackCommandService;
  }

  ListSourceStore& CoreRuntime::sources() noexcept
  {
    return _implPtr->listSourceStore;
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
