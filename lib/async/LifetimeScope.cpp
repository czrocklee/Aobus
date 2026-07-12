// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/async/LifetimeScope.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>

#include <cstdio> // NOLINT(misc-include-cleaner) -- directly provides stderr on MSVC
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <print>
#include <utility>
#include <vector>

namespace ao::async
{
  struct LifetimeScopeTask final
  {
    std::move_only_function<void()> cancel;
    bool completed{false};
  };

  LifetimeScope::LifetimeScope()
    : _statePtr{std::make_shared<LifetimeScopeState>()}
  {
  }

  LifetimeScope::~LifetimeScope()
  {
    cancelAll();
  }

  void LifetimeScope::cancelAll()
  {
    auto tasks = std::vector<std::shared_ptr<LifetimeScopeTask>>{};

    {
      auto lock = std::scoped_lock{_statePtr->mutex};
      _statePtr->isAlive = false;
      tasks = std::move(_statePtr->tasks);
    }

    for (auto const& taskPtr : tasks)
    {
      taskPtr->cancel();
    }
  }

  std::shared_ptr<LifetimeScopeState> LifetimeScope::state() const noexcept
  {
    return _statePtr;
  }

  namespace
  {
    void handleCoroutineCompletion(std::shared_ptr<LifetimeScopeState> statePtr,
                                   std::shared_ptr<LifetimeScopeTask> taskPtr,
                                   std::exception_ptr exPtr)
    {
      {
        auto lock = std::scoped_lock{statePtr->mutex};
        taskPtr->completed = true;
        std::erase(statePtr->tasks, taskPtr);
      }

      if (!exPtr)
      {
        return;
      }

      try
      {
        std::rethrow_exception(exPtr);
      }
      catch (std::exception const& ex)
      {
        if (isOperationCancelled(ex))
        {
          return;
        }

        std::println(stderr, "Unhandled exception in lifetime-bound coroutine: {}", ex.what());
      }
      catch (...)
      {
        std::println(stderr, "Unhandled unknown exception in lifetime-bound coroutine");
      }
    }
  } // namespace

  void Runtime::spawnWithLifetime(LifetimeScope* scope, CancellableTask task)
  {
    auto statePtr = scope->state();
    auto taskPtr = std::make_shared<LifetimeScopeTask>();
    taskPtr->cancel = startCancellable(
      std::move(task), [statePtr, taskPtr](auto exPtr) { handleCoroutineCompletion(statePtr, taskPtr, exPtr); });

    bool cancelImmediately = false;

    {
      auto lock = std::scoped_lock{statePtr->mutex};

      if (taskPtr->completed)
      {
        return;
      }

      if (statePtr->isAlive)
      {
        statePtr->tasks.push_back(taskPtr);
      }
      else
      {
        cancelImmediately = true;
      }
    }

    if (cancelImmediately)
    {
      taskPtr->cancel();
    }
  }
} // namespace ao::async
