// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/FrameTimer.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>

namespace ao::tui::test
{
  TEST_CASE("FrameSegmentStats - accumulates count, total, peak, and average", "[tui][unit][frame-timer]")
  {
    auto stats = FrameSegmentStats{};

    SECTION("empty stats report a zero average")
    {
      CHECK(stats.count == 0);
      CHECK(stats.averageDuration() == std::chrono::nanoseconds{0});
    }

    SECTION("samples fold into count, total, peak, and average")
    {
      stats.add(std::chrono::microseconds{10});
      stats.add(std::chrono::microseconds{30});
      stats.add(std::chrono::microseconds{20});

      CHECK(stats.count == 3);
      CHECK(stats.totalDuration == std::chrono::microseconds{60});
      CHECK(stats.peakDuration == std::chrono::microseconds{30});
      CHECK(stats.averageDuration() == std::chrono::microseconds{20});
    }
  }

  TEST_CASE("FrameTimingWindow - reports per interval and resets", "[tui][unit][frame-timer]")
  {
    auto window = FrameTimingWindow{3};

    SECTION("no report before the interval fills")
    {
      CHECK_FALSE(window.record(std::chrono::microseconds{1}, std::chrono::microseconds{2}).has_value());
      CHECK_FALSE(window.record(std::chrono::microseconds{1}, std::chrono::microseconds{2}).has_value());
    }

    SECTION("the interval frame emits a report with correct per-segment stats")
    {
      CHECK_FALSE(window.record(std::chrono::microseconds{10}, std::chrono::microseconds{100}).has_value());
      CHECK_FALSE(window.record(std::chrono::microseconds{20}, std::chrono::microseconds{300}).has_value());
      auto const optReport = window.record(std::chrono::microseconds{30}, std::chrono::microseconds{200});

      REQUIRE(optReport);
      CHECK(optReport->frames == 3);
      CHECK(optReport->build.averageDuration() == std::chrono::microseconds{20});
      CHECK(optReport->build.peakDuration == std::chrono::microseconds{30});
      CHECK(optReport->present.averageDuration() == std::chrono::microseconds{200});
      CHECK(optReport->present.peakDuration == std::chrono::microseconds{300});
    }

    SECTION("the window resets after a report so the next interval is independent")
    {
      window.record(std::chrono::microseconds{10}, std::chrono::microseconds{10});
      window.record(std::chrono::microseconds{10}, std::chrono::microseconds{10});
      REQUIRE(window.record(std::chrono::microseconds{10}, std::chrono::microseconds{10}).has_value());

      CHECK_FALSE(window.record(std::chrono::microseconds{50}, std::chrono::microseconds{60}).has_value());
      REQUIRE(window.pending().has_value());
      CHECK(window.pending()->frames == 1); // only the new window's frame is carried

      CHECK_FALSE(window.record(std::chrono::microseconds{50}, std::chrono::microseconds{60}).has_value());
      auto const optReport = window.record(std::chrono::microseconds{50}, std::chrono::microseconds{60});

      REQUIRE(optReport);
      CHECK(optReport->frames == 3);
      CHECK(optReport->build.averageDuration() == std::chrono::microseconds{50});
      CHECK(optReport->present.peakDuration == std::chrono::microseconds{60});
    }
  }

  TEST_CASE("FrameTimingWindow - pending reflects the in-progress window", "[tui][unit][frame-timer]")
  {
    auto window = FrameTimingWindow{4};

    CHECK_FALSE(window.pending().has_value());

    window.record(std::chrono::microseconds{5}, std::chrono::microseconds{7});
    window.record(std::chrono::microseconds{15}, std::chrono::microseconds{3});

    auto const optPending = window.pending();
    REQUIRE(optPending);
    CHECK(optPending->frames == 2);
    CHECK(optPending->build.peakDuration == std::chrono::microseconds{15});
    CHECK(optPending->present.averageDuration() == std::chrono::microseconds{5});
  }

  TEST_CASE("FrameTimingWindow - flush returns and resets a partial window", "[tui][unit][frame-timer]")
  {
    auto window = FrameTimingWindow{4};
    CHECK_FALSE(window.record(std::chrono::microseconds{5}, std::chrono::microseconds{7}).has_value());
    CHECK_FALSE(window.record(std::chrono::microseconds{15}, std::chrono::microseconds{3}).has_value());

    auto const optReport = window.flush();

    REQUIRE(optReport);
    CHECK(optReport->frames == 2);
    CHECK(optReport->build.averageDuration() == std::chrono::microseconds{10});
    CHECK(optReport->present.peakDuration == std::chrono::microseconds{7});
    CHECK_FALSE(window.pending().has_value());
    CHECK_FALSE(window.flush().has_value());
  }

  TEST_CASE("FrameTimer - disabled timer is inert and reports its state", "[tui][unit][frame-timer]")
  {
    auto timer = FrameTimer{false};

    CHECK_FALSE(timer.enabled());

    // Exercising the measurement path on a disabled timer must be a harmless no-op.
    {
      auto const scope = timer.measureBuild();
    }
    timer.recordPresentIfDrawn();

    auto const enabledTimer = FrameTimer{true};
    CHECK(enabledTimer.enabled());
  }
} // namespace ao::tui::test
