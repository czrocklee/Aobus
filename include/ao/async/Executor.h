// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/compat/MoveOnlyFunction.h>

namespace ao::async
{
  class Executor
  {
  public:
    virtual ~Executor() = default;

    Executor(Executor const&) = delete;
    Executor& operator=(Executor const&) = delete;
    Executor(Executor&&) = delete;
    Executor& operator=(Executor&&) = delete;

    virtual bool isCurrent() const noexcept = 0;

    // Thread-safe: run immediately when already on the owning thread; otherwise
    // enqueue and wake the executor's owning thread.
    virtual void dispatch(compat::MoveOnlyFunction<void()> task) = 0;

    // Always deferred: run in a later executor turn, even when already on the
    // owning thread. For a non-empty task, a normal return means the executor
    // accepted it. If this operation throws, the executor has not retained the
    // task and will not execute it.
    virtual void defer(compat::MoveOnlyFunction<void()> task) = 0;

  protected:
    Executor() = default;
  };
} // namespace ao::async
