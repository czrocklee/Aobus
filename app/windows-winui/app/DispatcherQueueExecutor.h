// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "app/DispatcherQueueAdmission.h"
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

    // Begin closure before stopping runtime producers. Closing still accepts
    // their final publications even when the native queue rejects a wake.
    void beginClosing() noexcept;
    // Call after every foreign producer has joined and while runtime callback
    // consumers are still alive.
    void completeClosing() noexcept;

    void dispatch(compat::MoveOnlyFunction<void()> task) override;
    void defer(compat::MoveOnlyFunction<void()> task) override;

  private:
    struct DispatchState final
    {
      std::atomic<DispatcherQueueExecutor*> executorPtr = nullptr;
    };

    void wake() noexcept override;

    detail::DispatcherQueueAdmission _admission;
    winrt::Microsoft::UI::Dispatching::DispatcherQueue _dispatcher{nullptr};
    std::shared_ptr<DispatchState> _dispatchStatePtr;
  };
} // namespace ao::winui
