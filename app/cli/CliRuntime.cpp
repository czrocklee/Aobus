// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CliRuntime.h"

#include <ao/async/Executor.h>
#include <ao/async/LoopExecutor.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/rt/CoreRuntime.h>
#include <ao/rt/library/Library.h>

#include <gsl-lite/gsl-lite.hpp>

#include <cstddef>
#include <exception>
#include <memory>
#include <ostream>
#include <utility>

namespace ao::cli
{
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

  CliRuntime::CliRuntime(std::ostream& out, std::ostream& err, std::size_t const musicLibraryMapSize)
    : _io{.out = out, .err = err}, _musicLibraryMapSize{musicLibraryMapSize}
  {
  }

  CliRuntime::~CliRuntime()
  {
    if (!_runtimePtr)
    {
      return;
    }

    auto& asyncRuntime = _runtimePtr->async();
    asyncRuntime.requestStop();
    asyncRuntime.join();

    while (true)
    {
      try
      {
        if (!_loopExecutor->runReadyTurn())
        {
          break;
        }
      }
      catch (...)
      {
        asyncRuntime.reportUnhandledException(std::current_exception(), "CLI callback executor shutdown");
      }
    }

    _runtimePtr.reset();
    _loopExecutor = nullptr;
  }

  rt::CoreRuntime& CliRuntime::core()
  {
    if (!_runtimePtr)
    {
      // CliRuntime is invocation-thread confined; lazy construction binds the
      // loop executor to the thread that enters the first command callback.
      auto executorPtr = std::make_unique<async::LoopExecutor>();
      auto* const loopExecutor = executorPtr.get();
      auto runtimePtr = std::make_unique<rt::CoreRuntime>(
        std::move(executorPtr), _options.root, _options.root / ".aobus/library", _musicLibraryMapSize);

      _loopExecutor = loopExecutor;
      _runtimePtr = std::move(runtimePtr);
    }

    return *_runtimePtr;
  }

  library::MusicLibrary& CliRuntime::musicLibrary()
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
    auto callbackExceptionPtr = std::exception_ptr{};

    while (!completionStatePtr->completed)
    {
      try
      {
        _loopExecutor->runOneTurn();
      }
      catch (...)
      {
        if (!callbackExceptionPtr)
        {
          callbackExceptionPtr = std::current_exception();
        }
        else
        {
          asyncRuntime.reportUnhandledException(std::current_exception(), "CLI callback executor");
        }
      }
    }

    try
    {
      completionFuture.get();
    }
    catch (...)
    {
      if (callbackExceptionPtr)
      {
        asyncRuntime.reportUnhandledException(callbackExceptionPtr, "CLI callback executor");
      }

      throw;
    }

    if (callbackExceptionPtr)
    {
      std::rethrow_exception(callbackExceptionPtr);
    }
  }
} // namespace ao::cli
