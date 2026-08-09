// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/async/QueuedExecutorBase.h>

#include <winrt/Microsoft.UI.Dispatching.h>

#include <atomic>
#include <memory>

namespace ao::winui
{
  class DispatcherQueueExecutor final : public async::QueuedExecutorBase
  {
  public:
    explicit DispatcherQueueExecutor(winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher);
    ~DispatcherQueueExecutor() override;

    DispatcherQueueExecutor(DispatcherQueueExecutor const&) = delete;
    DispatcherQueueExecutor& operator=(DispatcherQueueExecutor const&) = delete;
    DispatcherQueueExecutor(DispatcherQueueExecutor&&) = delete;
    DispatcherQueueExecutor& operator=(DispatcherQueueExecutor&&) = delete;

  private:
    struct DispatchState final
    {
      std::atomic<DispatcherQueueExecutor*> executorPtr = nullptr;
    };

    void wake() noexcept override;

    winrt::Microsoft::UI::Dispatching::DispatcherQueue _dispatcher{nullptr};
    std::shared_ptr<DispatchState> _dispatchStatePtr;
  };
} // namespace ao::winui
