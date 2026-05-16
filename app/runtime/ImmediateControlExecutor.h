// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "CorePrimitives.h"

namespace ao::rt
{
  /**
   * A control executor that runs all dispatched and deferred tasks
   * immediately on the current thread. Suitable for CLI and tests.
   */
  class ImmediateControlExecutor final : public IControlExecutor
  {
  public:
    ImmediateControlExecutor() = default;

    bool isCurrent() const noexcept override { return true; }

    void dispatch(std::move_only_function<void()> task) override { task(); }

    void defer(std::move_only_function<void()> task) override { task(); }
  };
} // namespace ao::rt
