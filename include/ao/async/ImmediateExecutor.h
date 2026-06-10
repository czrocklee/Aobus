// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/async/Executor.h>

#include <deque>
#include <functional>
#include <utility>

namespace ao::async
{
  // Runs everything inline on the calling thread, but still honors the defer contract:
  // a deferred task never starts inside the frame that scheduled it. Tasks are queued
  // and drained FIFO by the outermost defer call, so a task that defers further work
  // always finishes before that work begins.
  class ImmediateExecutor final : public IExecutor
  {
  public:
    ImmediateExecutor() = default;

    bool isCurrent() const noexcept override { return true; }

    void dispatch(std::move_only_function<void()> task) override { task(); }

    void defer(std::move_only_function<void()> task) override
    {
      _queue.push_back(std::move(task));

      if (_draining)
      {
        return;
      }

      _draining = true;

      try
      {
        while (!_queue.empty())
        {
          auto next = std::move(_queue.front());
          _queue.pop_front();
          next();
        }
      }
      catch (...)
      {
        // A throwing task ends this drain; tasks deferred after it stay queued for the
        // next executor turn.
        _draining = false;
        throw;
      }

      _draining = false;
    }

  private:
    std::deque<std::move_only_function<void()>> _queue;
    bool _draining = false;
  };
} // namespace ao::async
