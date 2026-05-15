// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/utility/Log.h>

#include <chrono>
#include <string>
#include <utility>

namespace ao::utility
{
  class ScopedTimer final
  {
  public:
    using Clock = std::chrono::steady_clock;

    explicit ScopedTimer(std::string label)
      : _label{std::move(label)}, _start{Clock::now()}
    {
    }

    ~ScopedTimer()
    {
      auto const end = Clock::now();
      auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - _start);
      APP_LOG_DEBUG("[perf] {} took {} ms", _label, elapsed.count());
    }

    ScopedTimer(ScopedTimer const&) = delete;
    ScopedTimer& operator=(ScopedTimer const&) = delete;
    ScopedTimer(ScopedTimer&&) = delete;
    ScopedTimer& operator=(ScopedTimer&&) = delete;

  private:
    std::string _label;
    Clock::time_point _start;
  };
}
