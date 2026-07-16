// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "Output.h"
#include <ao/Exception.h>
#include <ao/async/Task.h>

#include <cstddef>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace ao::async
{
  class LoopExecutor;
}

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
    explicit CliRuntime(std::ostream& out, std::ostream& err, std::size_t musicLibraryMapSize = 0);
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

      if (!optResult)
      {
        throwException<Exception>("CLI task completed without publishing its result");
      }

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

    CliOptions _options;
    CliIo _io;
    std::size_t _musicLibraryMapSize = 0;
    // Observes the executor owned through _runtimePtr.
    async::LoopExecutor* _loopExecutor = nullptr;
    std::unique_ptr<rt::CoreRuntime> _runtimePtr;
  };
} // namespace ao::cli
