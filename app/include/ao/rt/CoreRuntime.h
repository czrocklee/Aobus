// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <cstdint>
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
  class Sleeper;
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
    static Result<std::unique_ptr<CoreRuntime>> create(std::unique_ptr<async::Executor> executorPtr,
                                                       std::filesystem::path musicRoot,
                                                       std::filesystem::path databasePath,
                                                       std::uint64_t musicLibraryPinnedMapBytes = 0,
                                                       async::Sleeper* sleeper = nullptr);
    virtual ~CoreRuntime();

    // Composition-root teardown must not run from a synchronous runtime
    // observer. Defer shutdown to a later callback-executor turn instead.
    virtual void shutdown() noexcept;

    CoreRuntime(CoreRuntime const&) = delete;
    CoreRuntime& operator=(CoreRuntime const&) = delete;
    CoreRuntime(CoreRuntime&&) = delete;
    CoreRuntime& operator=(CoreRuntime&&) = delete;

    library::MusicLibrary const& musicLibrary() const noexcept;
    Library const& library() const noexcept;
    Library& library() noexcept;

    std::filesystem::path const& musicRoot() const noexcept;
    std::filesystem::path const& databasePath() const noexcept;

    CompletionService& completion() noexcept;
    TrackSourceCache& sources() noexcept;
    NotificationService& notifications() noexcept;

    async::Runtime& async() noexcept;

  protected:
    CoreRuntime();

    Result<> initialize(std::unique_ptr<async::Executor> executorPtr,
                        std::filesystem::path musicRoot,
                        std::filesystem::path databasePath,
                        std::uint64_t musicLibraryPinnedMapBytes,
                        async::Sleeper* sleeper);

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
