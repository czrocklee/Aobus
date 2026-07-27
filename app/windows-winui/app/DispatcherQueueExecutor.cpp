// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/DispatcherQueueExecutor.h"

#include <ao/Exception.h>
#include <ao/rt/Log.h>

#include <exception>
#include <functional>
#include <memory>

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
      APP_LOG_CRITICAL("DispatcherQueueExecutor: UI dispatcher rejected a callback after task admission");
      std::terminate();
    }
  }

  void DispatcherQueueExecutor::executeTask(std::move_only_function<void()>& task)
  {
    try
    {
      task();
    }
    catch (ao::Exception const& error)
    {
      APP_LOG_CRITICAL("DispatcherQueueExecutor: task failed: {} (at {}:{})", error.what(), error.file(), error.line());
    }
    catch (std::exception const& error)
    {
      APP_LOG_ERROR("DispatcherQueueExecutor: task failed: {}", error.what());
    }
    catch (...)
    {
      APP_LOG_ERROR("DispatcherQueueExecutor: task failed with an unknown exception");
    }
  }
} // namespace ao::winui
