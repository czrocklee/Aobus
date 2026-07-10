// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/rt/Log.h>

#include <chrono>
#include <string_view>

namespace ao::rt
{
  class [[nodiscard]] ScopedTimer final
  {
  public:
    using Clock = std::chrono::steady_clock;

    // The label is stored non-owning: callers must pass a string literal or another label
    // whose storage outlives the timer. This keeps construction allocation-free on the hot
    // measurement paths (a previous owning std::string copied/allocated every scope).
    explicit ScopedTimer(std::string_view label)
      : _label{label}, _start{Clock::now()}
    {
    }

    ~ScopedTimer()
    {
      auto const end = Clock::now();
      auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - _start);

      try
      {
        APP_LOG_DEBUG("[perf] {} took {} ms", _label, elapsed.count());
      }
      catch (...) // NOLINT(bugprone-empty-catch): destructor logging is best effort.
      {
        // Best-effort logging from a destructor; must not propagate.
      }
    }

    ScopedTimer(ScopedTimer const&) = delete;
    ScopedTimer& operator=(ScopedTimer const&) = delete;
    ScopedTimer(ScopedTimer&&) = delete;
    ScopedTimer& operator=(ScopedTimer&&) = delete;

  private:
    std::string_view _label;
    Clock::time_point _start;
  };
} // namespace ao::rt
