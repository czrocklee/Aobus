// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <filesystem>
#include <memory>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::rt::async
{
  class Runtime;
}

namespace ao::rt
{
  class IControlExecutor;
  class LibraryMutationService;
  class TrackCommandService;
  class ListSourceStore;
  class NotificationService;

  /**
   * The core application environment, containing frontend-neutral services
   * for data management and business logic.
   */
  class CoreRuntime
  {
  public:
    CoreRuntime(std::unique_ptr<IControlExecutor> executor, std::filesystem::path libraryRoot);
    virtual ~CoreRuntime();

    CoreRuntime(CoreRuntime const&) = delete;
    CoreRuntime& operator=(CoreRuntime const&) = delete;
    CoreRuntime(CoreRuntime&&) = delete;
    CoreRuntime& operator=(CoreRuntime&&) = delete;

    library::MusicLibrary& musicLibrary() noexcept;
    LibraryMutationService& mutation() noexcept;
    TrackCommandService& trackCommands() noexcept;
    ListSourceStore& sources() noexcept;
    NotificationService& notifications() noexcept;

    async::Runtime& async() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };
} // namespace ao::rt
