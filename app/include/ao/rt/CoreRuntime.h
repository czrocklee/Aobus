// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Task.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <vector>

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
  class CoreRuntime final
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
                                                       async::Sleeper* sleeper = nullptr,
                                                       TextOrderingPolicy const* textOrderingPolicy = nullptr,
                                                       CompletionAliasPolicy const* completionAliasPolicy = nullptr);
    ~CoreRuntime();

    // Composition-root teardown must not run from a synchronous runtime
    // observer. Defer shutdown to a later callback-executor turn instead.
    void shutdown() noexcept;

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
    /// CLI-only raw export path; uses the same verified walk without the interactive ceiling.
    async::Task<Result<std::optional<std::vector<std::byte>>>> loadResourceBytesForExportAsync(
      ResourceId resourceId,
      std::stop_token stopToken = {});

  private:
    friend class AppRuntime;

    struct Impl;
    explicit CoreRuntime(std::unique_ptr<Impl> implPtr);
    async::Task<Result<std::optional<std::vector<std::byte>>>> loadInteractiveResourceBytesAsync(
      ResourceId resourceId,
      std::stop_token stopToken);
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
