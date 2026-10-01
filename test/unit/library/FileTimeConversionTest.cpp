// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/library/FileTimeConversion.h>

#include "test/unit/FilesystemTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include <ao/Error.h>
#include <ao/FileTimestamp.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>

// NOLINTNEXTLINE(misc-include-cleaner) -- each standard library defines the macro in a different header.
#if __cpp_lib_chrono >= 201907L
#include "lib/library/FileClockConversion.h"

#include <catch2/catch_template_test_macros.hpp>

#include <ratio>
#include <type_traits>
#endif

namespace ao::library::test
{
  namespace
  {
    std::filesystem::file_time_type unixInstantToFileTime(std::int64_t const unixSeconds,
                                                          std::uint32_t const nanoseconds)
    {
      auto const wholeTime =
        ao::test::fileTimeFromSystemTime(std::chrono::sys_seconds{std::chrono::seconds{unixSeconds}});
      return wholeTime + std::chrono::duration_cast<std::filesystem::file_time_type::duration>(
                           std::chrono::nanoseconds{nanoseconds});
    }

#if __cpp_lib_chrono >= 201907L
    /** A file clock without to_sys() whose ticks count UTC time, leap seconds included, from 1970. */
    // The standard Cpp17Clock requirements fix these member names.
    // NOLINTBEGIN(readability-identifier-naming, aobus-readability-chrono-naming-convention)
    template<typename Period>
    struct UtcScaleClock final
    {
      using rep = std::int64_t;
      using period = Period;
      using duration = std::chrono::duration<rep, period>;
      using time_point = std::chrono::time_point<UtcScaleClock>;
      static constexpr bool is_steady = false;

      template<typename Duration>
      static auto to_utc(std::chrono::time_point<UtcScaleClock, Duration> const time)
      {
        return std::chrono::utc_time<std::common_type_t<Duration, std::chrono::seconds>>{time.time_since_epoch()};
      }
    };
    // NOLINTEND(readability-identifier-naming, aobus-readability-chrono-naming-convention)
#endif
  } // namespace

  TEST_CASE("FileTimeConversion - native modification times use Unix seconds and preserve the fraction",
            "[library][unit][file-timestamp]")
  {
    // These constants name 2024-07-01 12:00:59.500 UTC, independently of the
    // host's file-clock epoch and local zone.
    auto timestampRes = fileTimestampFromFileTime(unixInstantToFileTime(1719835259, 500000000));
    REQUIRE(timestampRes);
    CHECK(timestampRes->seconds == 1719835259);
    CHECK(timestampRes->nanoseconds == 500000000);
  }

  TEST_CASE("FileTimeConversion - epoch zero is a valid instant", "[library][unit][file-timestamp]")
  {
    auto timestampRes = fileTimestampFromFileTime(unixInstantToFileTime(0, 0));
    REQUIRE(timestampRes);
    CHECK(*timestampRes == FileTimestamp{.seconds = 0, .nanoseconds = 0});
  }

  TEST_CASE("FileTimeConversion - negative seconds retain a nonnegative fraction", "[library][unit][file-timestamp]")
  {
    // Half a second before the Unix epoch is (-1, 500000000), not (0, -500000000).
    auto timestampRes = fileTimestampFromFileTime(unixInstantToFileTime(-1, 500000000));
    REQUIRE(timestampRes);
    CHECK(timestampRes->seconds == -1);
    CHECK(timestampRes->nanoseconds == 500000000);
  }

  TEST_CASE("FileTimeConversion - dates beyond the Unix nanosecond range retain their instant",
            "[library][unit][file-timestamp]")
  {
    // 2300-01-01 00:00:00.750 UTC fits the supported file clocks but exceeds
    // signed 64-bit nanoseconds measured from 1970.
    auto timestampRes = fileTimestampFromFileTime(unixInstantToFileTime(10413792000, 750000000));
    REQUIRE(timestampRes);
    CHECK(timestampRes->seconds == 10413792000);
    CHECK(timestampRes->nanoseconds == 750000000);
  }

  TEST_CASE("FileTimeConversion - the native lower bound does not overflow while extracting the fraction",
            "[library][unit][file-timestamp]")
  {
    using FileDuration = std::filesystem::file_time_type::duration;
    using FileRep = FileDuration::rep;
    auto const minimumTime = std::filesystem::file_time_type::min();

    if constexpr (auto minimumRes = fileTimestampFromFileTime(minimumTime); sizeof(FileRep) <= sizeof(std::int64_t))
    {
      REQUIRE(minimumRes);
      CHECK(minimumRes->isNormalized());
      auto nextRes = fileTimestampFromFileTime(minimumTime + std::chrono::seconds{1});
      REQUIRE(nextRes);
      CHECK(nextRes->seconds == minimumRes->seconds + 1);
      CHECK(nextRes->nanoseconds == minimumRes->nanoseconds);
    }
    else
    {
      REQUIRE_FALSE(minimumRes);
      CHECK(minimumRes.error().code == Error::Code::ValueTooLarge);
    }
  }

  TEST_CASE("FileTimeConversion - a file's last write time reads as its Unix instant",
            "[library][unit][file-timestamp]")
  {
    auto const temp = ao::test::TempDir{};
    auto const path = temp.path() / "song.flac";
    std::ofstream{path} << "content";
    std::filesystem::last_write_time(path, unixInstantToFileTime(1719835259, 0));

    auto const timestampRes = lastWriteTimestamp(path);

    REQUIRE(timestampRes);
    CHECK(*timestampRes == FileTimestamp{.seconds = 1719835259, .nanoseconds = 0});
  }

  TEST_CASE("FileTimeConversion - a freshly written file reads near the system clock",
            "[library][unit][file-timestamp]")
  {
    // The filesystem, not a standard-library clock conversion, produces this
    // instant, so an epoch or leap-second offset in that conversion fails here.
    constexpr auto kAllowedOffset = std::chrono::seconds{2};
    auto const temp = ao::test::TempDir{};
    auto const path = temp.path() / "song.flac";
    auto const windowStart =
      std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now() - kAllowedOffset);
    std::ofstream{path} << "content";
    auto const windowEnd = std::chrono::ceil<std::chrono::seconds>(std::chrono::system_clock::now() + kAllowedOffset);

    auto const timestampRes = lastWriteTimestamp(path);

    REQUIRE(timestampRes);
    CHECK(timestampRes->seconds >= windowStart.time_since_epoch().count());
    CHECK(timestampRes->seconds <= windowEnd.time_since_epoch().count());
  }

#if __cpp_lib_chrono >= 201907L
  TEMPLATE_TEST_CASE("FileTimeConversion - a UTC-scale file clock maps leap seconds onto the Unix scale",
                     "[library][unit][file-timestamp]",
                     std::nano,
                     (std::ratio<1, 10'000'000>))
  {
    using Clock = UtcScaleClock<TestType>;
    using ClockDuration = Clock::duration;
    // 2017-01-01 00:00:00 UTC, immediately after the most recent inserted leap second.
    auto const leapEnd = std::chrono::utc_clock::from_sys(
      std::chrono::sys_days{std::chrono::year{2017} / std::chrono::January / std::chrono::day{1}});
    auto const atUtc = [](std::chrono::utc_time<ClockDuration> const utcTime)
    { return detail::fileTimestampFromClockTime(std::chrono::time_point<Clock>{utcTime.time_since_epoch()}); };

    SECTION("the second before the leap second keeps its fraction")
    {
      auto const timestampRes = atUtc(leapEnd - std::chrono::milliseconds{1500});
      REQUIRE(timestampRes);
      CHECK(*timestampRes == FileTimestamp{.seconds = 1483228799, .nanoseconds = 500000000});
    }

    SECTION("the inserted leap second clamps to the preceding second's last native tick")
    {
      auto const leapInstant = leapEnd - std::chrono::milliseconds{500};
      REQUIRE(std::chrono::get_leap_second_info(leapInstant).is_leap_second);

      auto const timestampRes = atUtc(leapInstant);

      REQUIRE(timestampRes);
      auto const nativeTickNs = std::chrono::duration_cast<std::chrono::nanoseconds>(ClockDuration{1}).count();
      CHECK(timestampRes->seconds == 1483228799);
      CHECK(timestampRes->nanoseconds == FileTimestamp::kNanosecondsPerSecond - nativeTickNs);
    }

    SECTION("the second after the leap second resumes the Unix scale")
    {
      auto const timestampRes = atUtc(leapEnd + std::chrono::milliseconds{250});
      REQUIRE(timestampRes);
      CHECK(*timestampRes == FileTimestamp{.seconds = 1483228800, .nanoseconds = 250000000});
    }
  }
#endif

  TEST_CASE("FileTimeConversion - a missing file reports an I/O error", "[library][unit][file-timestamp]")
  {
    auto const temp = ao::test::TempDir{};

    auto const timestampRes = lastWriteTimestamp(temp.path() / "missing.flac");

    REQUIRE_FALSE(timestampRes);
    CHECK(timestampRes.error().code == Error::Code::IoError);
  }

  TEST_CASE("FileTimestamp - comparison orders seconds before the normalized fraction",
            "[library][unit][file-timestamp]")
  {
    auto const beforeEpoch = FileTimestamp{.seconds = -1, .nanoseconds = 999999999};
    auto const epoch = FileTimestamp{.seconds = 0, .nanoseconds = 0};
    auto const afterEpoch = FileTimestamp{.seconds = 0, .nanoseconds = 1};
    CHECK(beforeEpoch < epoch);
    CHECK(epoch < afterEpoch);
    CHECK(afterEpoch != epoch);
  }

  TEST_CASE("FileTimestamp - a normalized fraction stays below one second", "[library][unit][file-timestamp]")
  {
    CHECK(FileTimestamp{.seconds = -1, .nanoseconds = FileTimestamp::kNanosecondsPerSecond - 1}.isNormalized());
    CHECK_FALSE(FileTimestamp{.seconds = 0, .nanoseconds = FileTimestamp::kNanosecondsPerSecond}.isNormalized());
  }
} // namespace ao::library::test
