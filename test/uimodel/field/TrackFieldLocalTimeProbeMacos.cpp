// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/fatal/ProbeProcess.h"

#include <stdlib.h> // NOLINT(modernize-deprecated-headers) -- POSIX setenv requires this header.
#include <time.h>   // NOLINT(modernize-deprecated-headers) -- POSIX tzset requires this header.

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <print>
#include <string_view>
#include <system_error>

int main(int argc, char* argv[])
{
  if (argc != 3 || std::string_view{argv[1]} != "--aobus-probe-child")
  {
    std::println(stderr, "Usage: ao_track_field_local_time_probe --aobus-probe-child <Unix seconds>");
    return 2;
  }

  ao::test::reportProbeChildStarted();
  auto const argument = std::string_view{argv[2]};
  std::time_t seconds = 0;
  auto const* const argumentEnd = argv[2] + argument.size();
  auto const [end, error] = std::from_chars(argv[2], argumentEnd, seconds);

  if (error != std::errc{} || end != argumentEnd)
  {
    std::println(stderr, "Invalid Unix seconds: {}", argument);
    return 2;
  }

  // ':' selects the operating system's compiled zone file. Only this child
  // overrides TZ, so no restoration is needed.
  // NOLINTNEXTLINE(misc-include-cleaner) -- Darwin stdlib.h reexports setenv from private _stdlib.h.
  if (::setenv("TZ", ":/etc/localtime", 1) != 0)
  {
    std::println(stderr, "Could not set oracle TZ");
    return 1;
  }

  // NOLINTNEXTLINE(misc-include-cleaner) -- Darwin time.h reexports tzset from private _time.h.
  ::tzset();
  auto local = std::tm{};

  if (::localtime_r(&seconds, &local) == nullptr)
  {
    std::println(stderr, "Could not convert Unix seconds: {}", seconds);
    return 1;
  }

  std::println("{:04}-{:02}-{:02} {:02}:{:02}",
               static_cast<std::int64_t>(local.tm_year) + 1900,
               local.tm_mon + 1,
               local.tm_mday,
               local.tm_hour,
               local.tm_min);
  return 0;
}
