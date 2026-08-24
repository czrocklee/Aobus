// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "Executor.h"
#include <ao/compat/MoveOnlyFunction.h>

#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>

namespace ao::async
{
  class QueuedExecutorBase : public Executor
  {
  public:
    ~QueuedExecutorBase() override = default;

    QueuedExecutorBase(QueuedExecutorBase const&) = delete;
    QueuedExecutorBase& operator=(QueuedExecutorBase const&) = delete;
    QueuedExecutorBase(QueuedExecutorBase&&) = delete;
    QueuedExecutorBase& operator=(QueuedExecutorBase&&) = delete;

    bool isCurrent() const noexcept override;
    void dispatch(compat::MoveOnlyFunction<void()> task) override;
    void defer(compat::MoveOnlyFunction<void()> task) override;

  protected:
    QueuedExecutorBase();

    void drainQueuedTasks();

  private:
    void enqueueAndWake(compat::MoveOnlyFunction<void()> task);

    // Admission is complete before wake. Event-loop wake failures are fatal
    // because an accepted task cannot be rolled back safely.
    virtual void wake() noexcept = 0;
    std::thread::id _ownerThread;
    std::mutex _mutex;
    std::vector<compat::MoveOnlyFunction<void()>> _pendingTasks;
    std::vector<compat::MoveOnlyFunction<void()>> _drainTasks;
    std::size_t _nextDrainTaskIndex = 0;
    bool _draining = false;
  };
} // namespace ao::async
