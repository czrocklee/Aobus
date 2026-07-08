// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <filesystem>
#include <memory>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::async
{
  class Executor;
  class Runtime;
}

namespace ao::rt
{
  class CompletionService;
  class Library;
  class TrackSourceCache;
  class NotificationService;

  /**
   * The core application environment, containing frontend-neutral services
   * for data management and business logic.
   */
  class CoreRuntime
  {
  public:
    CoreRuntime(std::unique_ptr<async::Executor> executorPtr,
                std::filesystem::path musicRoot,
                std::filesystem::path databasePath);
    virtual ~CoreRuntime();

    CoreRuntime(CoreRuntime const&) = delete;
    CoreRuntime& operator=(CoreRuntime const&) = delete;
    CoreRuntime(CoreRuntime&&) = delete;
    CoreRuntime& operator=(CoreRuntime&&) = delete;

    library::MusicLibrary& musicLibrary() noexcept;
    Library const& library() const noexcept;
    Library& library() noexcept;

    std::filesystem::path const& musicRoot() const noexcept;
    std::filesystem::path const& databasePath() const noexcept;

    CompletionService& completion() noexcept;
    TrackSourceCache& sources() noexcept;
    NotificationService& notifications() noexcept;

    async::Runtime& async() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
