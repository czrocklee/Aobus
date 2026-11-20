// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/async/LifetimeScope.h>

#include <ao/async/Runtime.h>
#include <ao/async/Task.h>

#include <exception>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::async
{
  struct LifetimeScope::State final
  {
    void retire(std::stop_source const& stopSource)
    {
      auto const lock = std::scoped_lock{mutex};
      std::ignore = std::erase(stopSources, stopSource);
    }

    std::mutex mutex;
    std::vector<std::stop_source> stopSources;
    bool isOpen{true};
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
    auto stopSources = std::vector<std::stop_source>{};

    {
      auto const lock = std::scoped_lock{_statePtr->mutex};
      _statePtr->isOpen = false;
      stopSources.swap(_statePtr->stopSources);
    }

    // request_stop() can run user callbacks synchronously, including reentrant cancellation.
    for (auto& stopSource : stopSources)
    {
      std::ignore = stopSource.request_stop();
    }
  }

  bool LifetimeScope::empty() const
  {
    auto const lock = std::scoped_lock{_statePtr->mutex};
    return _statePtr->stopSources.empty();
  }

  void Runtime::spawnWithLifetime(LifetimeScope& scope, CancellableTask task, std::string_view const fatalContext)
  {
    auto const statePtr = scope._statePtr;
    auto stopSource = std::stop_source{std::nostopstate};

    {
      auto const lock = std::scoped_lock{statePtr->mutex};

      if (!statePtr->isOpen)
      {
        return;
      }

      // Reserve admission and publish cancellation before submitting: co_spawn can run inline.
      stopSource = std::stop_source{};
      statePtr->stopSources.push_back(stopSource);
    }

    try
    {
      startCancellable(std::move(task),
                       stopSource.get_token(),
                       [statePtr, stopSource, fatalContext = std::string{fatalContext}](std::exception_ptr exceptionPtr)
                       {
                         statePtr->retire(stopSource);
                         finishFireAndForget(std::move(exceptionPtr), fatalContext);
                       });
    }
    catch (...)
    {
      statePtr->retire(stopSource);
      std::ignore = stopSource.request_stop();
      throw;
    }
  }
} // namespace ao::async
