// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/async/LifetimeScope.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>

#include <exception>
#include <functional>
#include <memory>
#include <mutex>
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
    void retireCoroutine(std::shared_ptr<LifetimeScopeState> const& statePtr,
                         std::shared_ptr<LifetimeScopeTask> const& taskPtr)
    {
      {
        auto lock = std::scoped_lock{statePtr->mutex};
        taskPtr->completed = true;
        std::erase(statePtr->tasks, taskPtr);
      }
    }
  } // namespace

  void Runtime::spawnWithLifetime(LifetimeScope* scope, CancellableTask task)
  {
    auto statePtr = scope->state();
    auto taskPtr = std::make_shared<LifetimeScopeTask>();
    auto diagnosticStatePtr = _diagnosticStatePtr;
    taskPtr->cancel = startCancellable(
      std::move(task),
      [diagnosticStatePtr = std::move(diagnosticStatePtr), statePtr, taskPtr](std::exception_ptr exceptionPtr)
      {
        retireCoroutine(statePtr, taskPtr);
        handleUnhandledException(*diagnosticStatePtr, std::move(exceptionPtr), "lifetime-bound coroutine");
      });

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
