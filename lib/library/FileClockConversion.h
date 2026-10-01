// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/FileTimestamp.h>

#include <chrono>
#include <cstdint>
#include <format>
#include <limits>
#include <stdexcept>

namespace ao::library::detail
{
  /**
   * Converts an instant of a file clock that provides either to_sys() or
   * to_utc(). Templated on the clock so tests can drive the UTC path, which the
   * host's file clock may not take.
   */
  template<typename FileClock>
  Result<FileTimestamp> fileTimestampFromClockTime(std::chrono::time_point<FileClock> const time)
  {
    constexpr std::int64_t kNanosecondsPerSecond = FileTimestamp::kNanosecondsPerSecond;

    auto const elapsed = time.time_since_epoch();
    auto wholeSeconds = elapsed / std::chrono::seconds{1};
    std::int64_t fractionNs =
      std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed % std::chrono::seconds{1}).count();

    // Normalize the bounded remainder instead of subtracting a floored time
    // point: converting that floor back to native ticks can overflow at min().
    if (fractionNs < 0)
    {
      --wholeSeconds;
      fractionNs += kNanosecondsPerSecond;
    }

    try
    {
      // Preserve a wider native representation through the seconds-domain
      // epoch shift; only its system-clock image is narrowed for storage.
      using ClockSeconds = std::chrono::duration<typename FileClock::duration::rep>;
      auto const wholeTime = std::chrono::time_point<FileClock, ClockSeconds>{ClockSeconds{wholeSeconds}};
      auto const systemTime = [&]
      {
        if constexpr (requires { FileClock::to_sys(wholeTime); })
        {
          return FileClock::to_sys(wholeTime);
        }
        else
        {
// NOLINTNEXTLINE(misc-include-cleaner) -- each standard library defines the macro in a different header.
#if __cpp_lib_chrono >= 201907L // libc++ lacks utc_clock; see doc/development/macos-portability.md
          auto const utcTime = FileClock::to_utc(wholeTime);

          // UTC-to-system conversion clamps an inserted leap second to the last
          // native tick of the preceding system second, not its original fraction.
          if (std::chrono::get_leap_second_info(utcTime).is_leap_second)
          {
            constexpr auto kNativeTick =
              std::chrono::duration_cast<std::chrono::nanoseconds>(typename FileClock::duration{1});
            static_assert(kNativeTick > std::chrono::nanoseconds::zero());
            fractionNs = kNanosecondsPerSecond - kNativeTick.count();
          }

          return std::chrono::utc_clock::to_sys(utcTime);
#else
          static_assert(false, "A file clock without to_sys() needs std::chrono::utc_clock");
#endif
        }
      }();

      auto const unixSeconds = systemTime.time_since_epoch().count();

      if (unixSeconds < std::numeric_limits<std::int64_t>::min() ||
          unixSeconds > std::numeric_limits<std::int64_t>::max())
      {
        return makeError(Error::Code::ValueTooLarge, "File modification time exceeds the Unix-seconds range");
      }

      return FileTimestamp{
        .seconds = static_cast<std::int64_t>(unixSeconds), .nanoseconds = static_cast<std::uint32_t>(fractionNs)};
    }
    catch (std::runtime_error const& error)
    {
      return makeError(
        Error::Code::IoError, std::format("Failed to convert file modification time to Unix time: {}", error.what()));
    }
  }
} // namespace ao::library::detail
