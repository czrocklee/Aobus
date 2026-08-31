// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CliRuntime.h"

#include "CommandError.h"
#include <ao/async/Executor.h>
#include <ao/async/LoopExecutor.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/rt/CoreRuntime.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryPaths.h>
#include <ao/utility/PlatformDirectories.h>

#include <gsl-lite/gsl-lite.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <ostream>
#include <utility>

namespace ao::cli
{
  struct CliRuntime::Storage final
  {
    std::optional<rt::CoreRuntime> optRuntime;
    async::LoopExecutor* loopExecutor = nullptr;
  };

  namespace
  {
    struct TaskCompletionState final
    {
      bool completed = false;
    };

    async::Task<void> publishTaskCompletion(async::Executor* executor,
                                            async::Task<void> task,
                                            std::shared_ptr<TaskCompletionState> completionStatePtr)
    {
      auto notifyCompletion =
        gsl_lite::finally([executor, completionStatePtr = std::move(completionStatePtr)]
                          { executor->dispatch([completionStatePtr] { completionStatePtr->completed = true; }); });
      co_await std::move(task);
    }
  } // namespace

  CliRuntime::CliRuntime(std::ostream& out,
                         std::ostream& err,
                         std::uint64_t const musicLibraryPinnedMapBytes,
                         std::optional<std::filesystem::path> optCacheDirectory)
    : _io{.out = out, .err = err}
    , _musicLibraryPinnedMapBytes{musicLibraryPinnedMapBytes}
    , _optCacheDirectory{std::move(optCacheDirectory)}
    , _storagePtr{std::make_unique<Storage>()}
  {
  }

  CliRuntime::~CliRuntime()
  {
    if (!_storagePtr->optRuntime)
    {
      return;
    }

    _storagePtr->optRuntime->shutdown();

    while (_storagePtr->loopExecutor->runReadyTurn())
    {
    }

    _storagePtr->optRuntime.reset();
    _storagePtr->loopExecutor = nullptr;
  }

  rt::CoreRuntime& CliRuntime::core()
  {
    if (!_storagePtr->optRuntime)
    {
      // CliRuntime is invocation-thread confined; lazy construction binds the
      // loop executor to the thread that enters the first command callback.
      auto executorPtr = std::make_unique<async::LoopExecutor>();
      auto* const loopExecutor = executorPtr.get();

      // This is the only platform location the CLI resolves; it resolves its
      // music root and nothing else. An absent one costs nothing: cover reads
      // then re-extract from the media files on every request.
      auto const cacheDirectory = [this]
      {
        if (_optCacheDirectory)
        {
          return *_optCacheDirectory;
        }

        auto const cacheDirRes = utility::applicationCacheDirectory();
        return cacheDirRes ? *cacheDirRes : std::filesystem::path{};
      }();
      auto runtimeRes = rt::CoreRuntime::create(std::move(executorPtr),
                                                _options.root,
                                                rt::LibraryPaths{_options.root}.databasePath(),
                                                cacheDirectory,
                                                _musicLibraryPinnedMapBytes);

      if (!runtimeRes)
      {
        throwCommandError(runtimeRes.error(), "failed to open library: {}", runtimeRes.error().message);
      }

      _storagePtr->optRuntime.emplace(std::move(*runtimeRes));
      _storagePtr->loopExecutor = loopExecutor;
    }

    return *_storagePtr->optRuntime;
  }

  library::MusicLibrary const& CliRuntime::musicLibrary()
  {
    return core().musicLibrary();
  }

  rt::Library& CliRuntime::library()
  {
    return core().library();
  }

  void CliRuntime::runTask(async::Task<void> task)
  {
    auto& asyncRuntime = core().async();
    auto completionStatePtr = std::make_shared<TaskCompletionState>();
    auto completionFuture =
      asyncRuntime.spawn(publishTaskCompletion(&asyncRuntime.callbackExecutor(), std::move(task), completionStatePtr));

    while (!completionStatePtr->completed)
    {
      _storagePtr->loopExecutor->runOneTurn();
    }

    completionFuture.get();
  }
} // namespace ao::cli
