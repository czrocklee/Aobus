// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/async/Executor.h>

#include <functional>

namespace ao::async
{
  class ImmediateExecutor final : public IExecutor
  {
  public:
    ImmediateExecutor() = default;

    bool isCurrent() const noexcept override { return true; }

    void dispatch(std::move_only_function<void()> task) override { task(); }

    void defer(std::move_only_function<void()> task) override { task(); }
  };
} // namespace ao::async
