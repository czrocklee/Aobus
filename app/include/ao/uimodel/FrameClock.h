// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <chrono>
#include <cstdint>
#include <ratio>

namespace ao::uimodel
{
  /**
   * @brief Tag clock for a frontend's monotonic frame-presentation timeline
   *        expressed in microseconds.
   *
   * It is deliberately a distinct type from std::chrono::steady_clock so that
   * frame timestamps cannot be accidentally mixed with steady_clock::now(), and
   * so that subtracting two frame timestamps is the only meaningful arithmetic
   * (it yields a Duration, while adding two timestamps fails to compile).
   *
   * This is not a full Clock: it exposes no now(). Each frontend supplies one
   * consistent timeline, such as Gdk frame time or steady-clock time, for all
   * updates and samples of an interpolator; epochs from different sources must
   * not be mixed.
   */
  struct FrameClock final
  {
    using Duration = std::chrono::duration<std::int64_t, std::micro>;
    using TimePoint = std::chrono::time_point<FrameClock, Duration>;

    static constexpr TimePoint fromMicros(std::int64_t micros) noexcept { return TimePoint{Duration{micros}}; }
  };
} // namespace ao::uimodel
