// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <functional>

namespace ao::async
{
  class IExecutor
  {
  public:
    virtual ~IExecutor() = default;

    IExecutor(IExecutor const&) = delete;
    IExecutor& operator=(IExecutor const&) = delete;
    IExecutor(IExecutor&&) = delete;
    IExecutor& operator=(IExecutor&&) = delete;

    virtual bool isCurrent() const noexcept = 0;

    // Thread-safe: run immediately when already on the owning thread; otherwise
    // enqueue and wake the executor's owning thread.
    virtual void dispatch(std::move_only_function<void()> task) = 0;

    // Always deferred: run in a later executor turn, even when already on the owning thread.
    virtual void defer(std::move_only_function<void()> task) = 0;

  protected:
    IExecutor() = default;
  };
} // namespace ao::async
