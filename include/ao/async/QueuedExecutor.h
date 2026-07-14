// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "Executor.h"

#include <functional>
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
    void dispatch(std::move_only_function<void()> task) override;
    void defer(std::move_only_function<void()> task) override;

  protected:
    QueuedExecutorBase();

    void drainQueuedTasks();

  private:
    void enqueueAndWake(std::move_only_function<void()> task);

    virtual void wake() = 0;
    virtual void executeTask(std::move_only_function<void()>& task) = 0;

    std::thread::id _ownerThread;
    std::mutex _mutex;
    std::vector<std::move_only_function<void()>> _pendingTasks;
    std::vector<std::move_only_function<void()>> _drainTasks;
    bool _draining = false;
  };
} // namespace ao::async
