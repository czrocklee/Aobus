// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CoreRuntime.h"

#include "LibraryMutationService.h"
#include "ListSourceStore.h"
#include "NotificationService.h"
#include "TrackCommandService.h"
#include "ao/library/MusicLibrary.h"
#include "async/Runtime.h"

#include <filesystem>
#include <memory>
#include <utility>

namespace ao::rt
{
  struct CoreRuntime::Impl final
  {
    std::unique_ptr<IControlExecutor> executor;
    async::Runtime asyncRuntime;
    library::MusicLibrary musicLibrary;
    LibraryMutationService mutationService;
    TrackCommandService trackCommandService;
    ListSourceStore listSourceStore;
    NotificationService notificationService;

    Impl(std::unique_ptr<IControlExecutor> exec, std::filesystem::path libraryRoot)
      : executor{std::move(exec)}
      , asyncRuntime{*executor}
      , musicLibrary{std::move(libraryRoot)}
      , mutationService{asyncRuntime, musicLibrary}
      , trackCommandService{musicLibrary, mutationService}
      , listSourceStore{musicLibrary, mutationService}
      , notificationService{}
    {
    }
  };

  CoreRuntime::CoreRuntime(std::unique_ptr<IControlExecutor> executor, std::filesystem::path libraryRoot)
    : _impl{std::make_unique<Impl>(std::move(executor), std::move(libraryRoot))}
  {
  }

  CoreRuntime::~CoreRuntime() = default;

  library::MusicLibrary& CoreRuntime::musicLibrary() noexcept
  {
    return _impl->musicLibrary;
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
