// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/async/LifetimeScope.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>

#include <cstdio> // NOLINT(misc-include-cleaner) -- directly provides stderr on MSVC
#include <exception>
#include <memory>
#include <mutex>
#include <print>
#include <utility>

namespace ao::async
{
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
    auto lock = std::scoped_lock{_statePtr->mutex};
    _statePtr->isAlive = false;

    for (auto const& signalPtr : _statePtr->signals)
    {
      signalPtr->emit(CancellationType::all);
    }
  }

  std::shared_ptr<LifetimeScopeState> LifetimeScope::state() const noexcept
  {
    return _statePtr;
  }

  namespace
  {
    void handleCoroutineCompletion(std::shared_ptr<LifetimeScopeState> statePtr,
                                   std::shared_ptr<CancellationSignal> signalPtr,
                                   std::exception_ptr exPtr)
    {
      {
        auto lock = std::scoped_lock{statePtr->mutex};

        if (statePtr->isAlive)
        {
          std::erase(statePtr->signals, signalPtr);
        }
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

  void Runtime::spawnWithLifetime(LifetimeScope* scope, Task<void> task)
  {
    auto signalPtr = std::make_shared<CancellationSignal>();
    auto statePtr = scope->state();

    {
      auto lock = std::scoped_lock{statePtr->mutex};

      if (!statePtr->isAlive)
      {
        return;
      }

      statePtr->signals.push_back(signalPtr);
    }

    spawn(std::move(task),
          signalPtr->slot(),
          [statePtr, signalPtr](auto exPtr) { handleCoroutineCompletion(statePtr, signalPtr, exPtr); });
  }
} // namespace ao::async
