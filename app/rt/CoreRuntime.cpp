// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/CoreRuntime.h>

#include <ao/rt/LibraryMutationService.h>
#include <ao/rt/ListSourceStore.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/TrackCommandService.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/async/Runtime.h>

#include <filesystem>
#include <memory>
#include <utility>

namespace ao::rt
{
  struct CoreRuntime::Impl final
  {
    std::unique_ptr<IControlExecutor> executor;
    async::Runtime asyncRuntime;
    std::filesystem::path musicRoot;
    std::filesystem::path databasePath;
    library::MusicLibrary musicLibrary;
    LibraryMutationService mutationService;
    TrackCommandService trackCommandService;
    ListSourceStore listSourceStore;
    NotificationService notificationService;

    Impl(std::unique_ptr<IControlExecutor> exec, std::filesystem::path musicRoot, std::filesystem::path databasePath)
      : executor{std::move(exec)}
      , asyncRuntime{*executor}
      , musicRoot{std::move(musicRoot)}
      , databasePath{std::move(databasePath)}
      , musicLibrary{this->musicRoot, this->databasePath}
      , mutationService{asyncRuntime, musicLibrary}
      , trackCommandService{musicLibrary, mutationService}
      , listSourceStore{musicLibrary, mutationService}
      , notificationService{}
    {
    }
  };

  CoreRuntime::CoreRuntime(std::unique_ptr<IControlExecutor> executor,
                           std::filesystem::path musicRoot,
                           std::filesystem::path databasePath)
    : _impl{std::make_unique<Impl>(std::move(executor), std::move(musicRoot), std::move(databasePath))}
  {
  }

  CoreRuntime::~CoreRuntime() = default;

  library::MusicLibrary& CoreRuntime::musicLibrary() noexcept
  {
    return _impl->musicLibrary;
  }

  std::filesystem::path const& CoreRuntime::musicRoot() const noexcept
  {
    return _impl->musicRoot;
  }

  std::filesystem::path const& CoreRuntime::databasePath() const noexcept
  {
    return _impl->databasePath;
  }

  LibraryMutationService& CoreRuntime::mutation() noexcept
  {
    return _impl->mutationService;
  }

  TrackCommandService& CoreRuntime::trackCommands() noexcept
  {
    return _impl->trackCommandService;
  }

  ListSourceStore& CoreRuntime::sources() noexcept
  {
    return _impl->listSourceStore;
  }

  NotificationService& CoreRuntime::notifications() noexcept
  {
    return _impl->notificationService;
  }

  async::Runtime& CoreRuntime::async() noexcept
  {
    return _impl->asyncRuntime;
  }
} // namespace ao::rt
