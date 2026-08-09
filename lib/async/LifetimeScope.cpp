// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/async/LifetimeScope.h>

#include <ao/async/Runtime.h>
#include <ao/async/Task.h>

#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::async
{
  struct LifetimeScopeTask final
  {
    std::move_only_function<void()> cancel;
    bool completed{false};
  };

  struct LifetimeScope::State final
  {
    std::mutex mutex;
    std::vector<std::shared_ptr<LifetimeScopeTask>> tasks;
    bool isAlive{true};
  };

  LifetimeScope::LifetimeScope()
    : _statePtr{std::make_shared<State>()}
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

  bool LifetimeScope::empty() const
  {
    auto const lock = std::scoped_lock{_statePtr->mutex};
    return _statePtr->tasks.empty();
  }

  void Runtime::spawnWithLifetime(LifetimeScope* scope, CancellableTask task, std::string_view const fatalContext)
  {
    auto statePtr = scope->_statePtr;
    auto taskPtr = std::make_shared<LifetimeScopeTask>();
    taskPtr->cancel =
      startCancellable(std::move(task),
                       [statePtr, taskPtr, fatalContext = std::string{fatalContext}](std::exception_ptr exceptionPtr)
                       {
                         {
                           auto lock = std::scoped_lock{statePtr->mutex};
                           taskPtr->completed = true;
                           std::erase(statePtr->tasks, taskPtr);
                         }

                         finishFireAndForget(std::move(exceptionPtr), fatalContext);
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
