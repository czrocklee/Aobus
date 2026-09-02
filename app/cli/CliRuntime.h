// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "Output.h"
#include <ao/Contract.h>
#include <ao/async/Task.h>

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::rt
{
  class CoreRuntime;
  class Library;
}

namespace ao::cli
{
  struct CliOptions final
  {
    std::filesystem::path root{"."};
    OutputFormat format = OutputFormat::Plain;
  };

  struct CliIo final
  {
    std::ostream& out;
    std::ostream& err;
  };

  class CliRuntime final
  {
  public:
    /**
     * @param optCacheDirectory Where derived caches live, or nothing to resolve
     * the platform location. A supplied one is how a test keeps the machine's
     * real cover cache out of its results, and out of its reach.
     */
    explicit CliRuntime(std::ostream& out,
                        std::ostream& err,
                        std::uint64_t musicLibraryPinnedMapBytes = 0,
                        std::optional<std::filesystem::path> optCacheDirectory = std::nullopt);
    ~CliRuntime();

    CliRuntime(CliRuntime const&) = delete;
    CliRuntime& operator=(CliRuntime const&) = delete;
    CliRuntime(CliRuntime&&) = delete;
    CliRuntime& operator=(CliRuntime&&) = delete;

    rt::CoreRuntime& core();
    library::MusicLibrary const& musicLibrary();
    rt::Library& library();

    void runTask(async::Task<void> task);

    template<typename T>
      requires(!std::is_void_v<T>)
    T runTask(async::Task<T> task)
    {
      auto optResultPtr = std::make_shared<std::optional<T>>();
      runTask(storeTaskResult(std::move(task), optResultPtr));
      auto optResult = std::move(*optResultPtr);

      AO_INVARIANT(optResult, "CLI task completed without publishing its result");

      return std::move(*optResult);
    }

    CliOptions& options() noexcept { return _options; }
    CliOptions const& options() const noexcept { return _options; }
    CliIo& io() noexcept { return _io; }
    CliIo const& io() const noexcept { return _io; }

  private:
    template<typename T>
    static async::Task<void> storeTaskResult(async::Task<T> task, std::shared_ptr<std::optional<T>> optResultPtr)
    {
      optResultPtr->emplace(co_await std::move(task));
    }

    struct Storage;

    CliOptions _options;
    CliIo _io;
    std::uint64_t _musicLibraryPinnedMapBytes = 0;
    std::optional<std::filesystem::path> _optCacheDirectory;
    std::unique_ptr<Storage> _storagePtr;
  };
} // namespace ao::cli
