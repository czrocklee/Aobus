// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <chrono>
#include <cstdint>
#include <ratio>

namespace ao::uimodel
{
  /**
   * @brief Tag clock modeling Gdk::FrameClock's monotonic frame-presentation
   *        timeline (g_get_monotonic_time, microseconds).
   *
   * It is deliberately a distinct type from std::chrono::steady_clock so that
   * frame timestamps cannot be accidentally mixed with steady_clock::now(), and
   * so that subtracting two frame timestamps is the only meaningful arithmetic
   * (it yields a Duration, while adding two timestamps fails to compile).
   *
   * This is not a full Clock: it exposes no now() because every value originates
   * from Gdk; only time_point arithmetic is used.
   */
  struct FrameClock final
  {
    using Duration = std::chrono::duration<std::int64_t, std::micro>;
    using TimePoint = std::chrono::time_point<FrameClock, Duration>;

    static constexpr TimePoint fromMicros(std::int64_t micros) noexcept { return TimePoint{Duration{micros}}; }
  };
} // namespace ao::uimodel
