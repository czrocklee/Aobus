// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/library/FileTimestamp.h>

#include <ao/Error.h>
#include <ao/FileTimestamp.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>

namespace ao::library::test
{
  namespace
  {
    std::filesystem::file_time_type unixInstantToFileTime(std::int64_t const unixSeconds,
                                                          std::uint32_t const nanoseconds)
    {
      auto const wholeTime =
        std::chrono::clock_cast<std::chrono::file_clock>(std::chrono::sys_seconds{std::chrono::seconds{unixSeconds}});
      using FileDuration = std::filesystem::file_time_type::duration;
      return std::filesystem::file_time_type{
        std::chrono::duration_cast<FileDuration>(wholeTime.time_since_epoch()) +
        std::chrono::duration_cast<FileDuration>(std::chrono::nanoseconds{nanoseconds})};
    }
  } // namespace

  TEST_CASE("FileTimestamp - native modification times use Unix seconds and preserve the fraction",
            "[library][unit][file-timestamp]")
  {
    // These constants name 2024-07-01 12:00:59.500 UTC, independently of the
    // host's file-clock epoch and local zone.
    auto timestampRes = fileTimestampFromFileTime(unixInstantToFileTime(1719835259, 500000000));
    REQUIRE(timestampRes);
    CHECK(timestampRes->seconds == 1719835259);
    CHECK(timestampRes->nanoseconds == 500000000);
  }

  TEST_CASE("FileTimestamp - epoch zero is a valid instant", "[library][unit][file-timestamp]")
  {
    auto timestampRes = fileTimestampFromFileTime(unixInstantToFileTime(0, 0));
    REQUIRE(timestampRes);
    CHECK(*timestampRes == FileTimestamp{.seconds = 0, .nanoseconds = 0});
  }

  TEST_CASE("FileTimestamp - negative seconds retain a nonnegative fraction", "[library][unit][file-timestamp]")
  {
    // Half a second before the Unix epoch is (-1, 500000000), not (0, -500000000).
    auto timestampRes = fileTimestampFromFileTime(unixInstantToFileTime(-1, 500000000));
    REQUIRE(timestampRes);
    CHECK(timestampRes->seconds == -1);
    CHECK(timestampRes->nanoseconds == 500000000);
  }

  TEST_CASE("FileTimestamp - dates beyond the Unix nanosecond range retain their instant",
            "[library][unit][file-timestamp]")
  {
    // 2300-01-01 00:00:00.750 UTC fits the supported file clocks but exceeds
    // signed 64-bit nanoseconds measured from 1970.
    auto timestampRes = fileTimestampFromFileTime(unixInstantToFileTime(10413792000, 750000000));
    REQUIRE(timestampRes);
    CHECK(timestampRes->seconds == 10413792000);
    CHECK(timestampRes->nanoseconds == 750000000);
  }

  TEST_CASE("FileTimestamp - the native lower bound does not overflow while extracting the fraction",
            "[library][unit][file-timestamp]")
  {
    using FileDuration = std::filesystem::file_time_type::duration;
    using FileRep = FileDuration::rep;
    auto const minimumTime = std::filesystem::file_time_type::min();

    if constexpr (auto minimumRes = fileTimestampFromFileTime(minimumTime); sizeof(FileRep) <= sizeof(std::int64_t))
    {
      REQUIRE(minimumRes);
      CHECK(minimumRes->nanoseconds < 1000000000);
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
} // namespace ao::library::test
