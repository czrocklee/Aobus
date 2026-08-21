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
  class CompletionAliasPolicy;
  class CompletionService;
  class Library;
  class TrackSourceCache;
  class NotificationService;
  class TextOrderingPolicy;

  /**
   * The core application environment, containing frontend-neutral services
   * for data management and business logic.
   */
  class CoreRuntime
  {
  public:
    /// @param cacheDirectory Where derived caches live, resolved by the
    ///        composition root: the runtime owns paths derived from a supplied
    ///        root and does not discover platform application directories. Empty
    ///        is supported: a cover read then re-extracts from a carrier file
    ///        every time, which costs latency, and costs the image itself for
    ///        content whose carrier files are all gone.
    static Result<std::unique_ptr<CoreRuntime>> create(std::unique_ptr<async::Executor> executorPtr,
                                                       std::filesystem::path musicRoot,
                                                       std::filesystem::path databasePath,
                                                       std::filesystem::path cacheDirectory = {},
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
    TextOrderingPolicy const* textOrderingPolicy() const noexcept;

    async::Runtime& async() noexcept;

  protected:
    CoreRuntime();

    Result<> initialize(std::unique_ptr<async::Executor> executorPtr,
                        std::filesystem::path musicRoot,
                        std::filesystem::path databasePath,
                        std::filesystem::path cacheDirectory,
                        std::uint64_t musicLibraryPinnedMapBytes,
                        async::Sleeper* sleeper,
                        TextOrderingPolicy const* textOrderingPolicy = nullptr,
                        CompletionAliasPolicy const* completionAliasPolicy = nullptr);

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
