// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/library/FileTimestamp.h>

#include <ao/Error.h>
#include <ao/FileTimestamp.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <limits>
#include <stdexcept>

namespace ao::library
{
  namespace
  {
    constexpr std::int64_t kNanosecondsPerSecond = 1000000000;

    template<typename FileClock>
    auto wholeFileSecondsToSystemTime(
      std::chrono::file_time<std::chrono::duration<typename FileClock::duration::rep>> const wholeTime,
      std::int64_t& fractionNs)
    {
      if constexpr (requires { FileClock::to_sys(wholeTime); })
      {
        return FileClock::to_sys(wholeTime);
      }
      else
      {
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
      }
    }
  } // namespace

  Result<FileTimestamp> fileTimestampFromFileTime(std::filesystem::file_time_type const time)
  {
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
      using FileSeconds = std::chrono::duration<std::filesystem::file_time_type::duration::rep>;
      auto const wholeTime = std::chrono::file_time<FileSeconds>{FileSeconds{wholeSeconds}};
      auto const systemTime = wholeFileSecondsToSystemTime<std::chrono::file_clock>(wholeTime, fractionNs);
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
} // namespace ao::library
