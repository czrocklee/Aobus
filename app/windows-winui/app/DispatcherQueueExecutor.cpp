// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/DispatcherQueueExecutor.h"

#include <ao/Contract.h>

#include <exception>
#include <memory>
#include <utility>

namespace ao::winui
{
  DispatcherQueueExecutor::DispatcherQueueExecutor(winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher)
    : _dispatcher{std::move(dispatcher)}, _dispatchStatePtr{std::make_shared<DispatchState>()}
  {
    _dispatchStatePtr->executorPtr.store(this);
  }

  DispatcherQueueExecutor::~DispatcherQueueExecutor()
  {
    _dispatchStatePtr->executorPtr.store(nullptr);
  }

  void DispatcherQueueExecutor::wake() noexcept
  {
    try
    {
      auto statePtr = _dispatchStatePtr;
      auto const queued = _dispatcher.TryEnqueue(
        [statePtr = std::move(statePtr)]
        {
          if (auto* const executor = statePtr->executorPtr.load(); executor != nullptr)
          {
            executor->drainQueuedTasks();
          }
        });

      if (!queued)
      {
        AO_FATAL("Windows dispatcher rejected a callback after executor admission");
      }
    }
    catch (...)
    {
      AO_FATAL_EXCEPTION(std::current_exception(), "Windows dispatcher wake");
    }
  }
} // namespace ao::winui
