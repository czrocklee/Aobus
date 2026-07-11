// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/playback/PlaybackRestartDeadline.h"

#include "test/unit/RuntimeTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    using Elapsed = PlaybackRestartDeadline::Elapsed;
    using TimePoint = PlaybackRestartDeadline::TimePoint;

    class FakeRestartDeadlineScheduler final : public PlaybackRestartDeadline::Scheduler
    {
    public:
      using DeadlineCallback = PlaybackRestartDeadline::DeadlineCallback;

      struct ScheduledCall final
      {
        TimePoint deadline{};
        DeadlineCallback callback;
        bool cancelled = false;
      };

      void schedule(TimePoint const deadline, DeadlineCallback callback) override
      {
        _calls.push_back(ScheduledCall{.deadline = deadline, .callback = std::move(callback)});
        _optActiveIndex = _calls.size() - 1;
      }

      void cancel() noexcept override
      {
        if (_optActiveIndex)
        {
          _calls[*_optActiveIndex].cancelled = true;
          _optActiveIndex.reset();
        }
      }

      void fire(std::size_t const index)
      {
        auto callback = std::move(_calls.at(index).callback);

        if (_optActiveIndex == index)
        {
          _optActiveIndex.reset();
        }

        callback();
      }

      std::size_t callCount() const noexcept { return _calls.size(); }
      ScheduledCall const& call(std::size_t const index) const { return _calls.at(index); }

    private:
      std::vector<ScheduledCall> _calls;
      std::optional<std::size_t> _optActiveIndex;
    };

    struct ControlledClock final
    {
      TimePoint currentTime{};

      void advance(Elapsed const duration) { currentTime += duration; }
    };

    class RestartDeadlineFixture final
    {
    public:
      RestartDeadlineFixture()
        : deadline{executor,
                   scheduler,
                   [this] { return clock.currentTime; },
                   [this]
                   {
                     ++liveElapsedReadCount;
                     return liveElapsed;
                   },
                   [this](bool const available) { availabilityEvents.push_back(available); }}
      {
      }

      // These fixture values are intentionally public as the tests' assertion surface.
      // NOLINTBEGIN(aobus-readability-identifier-naming-extensions)
      ManualExecutor executor;
      FakeRestartDeadlineScheduler scheduler;
      ControlledClock clock;
      Elapsed liveElapsed{0};
      std::size_t liveElapsedReadCount = 0;
      std::vector<bool> availabilityEvents;
      PlaybackRestartDeadline deadline;
      // NOLINTEND(aobus-readability-identifier-naming-extensions)
    };
  } // namespace

  TEST_CASE("PlaybackRestartDeadline - equality remains unavailable and an early callback retries",
            "[runtime][unit][playback-cursor]")
  {
    auto fixture = RestartDeadlineFixture{};

    fixture.deadline.start(Elapsed{2999});

    CHECK(fixture.deadline.isActive());
    CHECK(fixture.deadline.isRunning());
    CHECK_FALSE(fixture.deadline.restartAvailable());
    CHECK(fixture.deadline.hasScheduledDeadline());
    REQUIRE(fixture.scheduler.callCount() == 1);
    CHECK(fixture.scheduler.call(0).deadline == TimePoint{} + Elapsed{2});

    fixture.clock.advance(Elapsed{2});
    fixture.liveElapsed = Elapsed{3000};
    fixture.scheduler.fire(0);

    CHECK(fixture.executor.queuedCount() == 1);
    CHECK(fixture.liveElapsedReadCount == 0);
    CHECK(fixture.availabilityEvents.empty());
    REQUIRE(fixture.executor.runOne());
    CHECK(fixture.liveElapsedReadCount == 1);
    CHECK_FALSE(fixture.deadline.restartAvailable());
    REQUIRE(fixture.scheduler.callCount() == 2);
    CHECK(fixture.scheduler.call(1).deadline == fixture.clock.currentTime + Elapsed{1});

    fixture.clock.advance(Elapsed{1});
    fixture.liveElapsed = Elapsed{3001};
    fixture.scheduler.fire(1);
    CHECK_FALSE(fixture.deadline.restartAvailable());
    CHECK(fixture.executor.queuedCount() == 1);

    REQUIRE(fixture.executor.runOne());
    CHECK(fixture.deadline.restartAvailable());
    CHECK_FALSE(fixture.deadline.hasScheduledDeadline());
    CHECK(fixture.availabilityEvents == std::vector{true});
  }

  TEST_CASE("PlaybackRestartDeadline - queued callbacks from an older reschedule generation are ignored",
            "[runtime][unit][playback-cursor]")
  {
    auto fixture = RestartDeadlineFixture{};
    fixture.deadline.start(Elapsed{0});
    REQUIRE(fixture.scheduler.callCount() == 1);
    fixture.scheduler.fire(0);
    REQUIRE(fixture.executor.queuedCount() == 1);

    fixture.deadline.seek(Elapsed{1000});
    REQUIRE(fixture.scheduler.callCount() == 2);
    fixture.liveElapsed = Elapsed{3001};

    REQUIRE(fixture.executor.runOne());
    CHECK(fixture.liveElapsedReadCount == 0);
    CHECK(fixture.availabilityEvents.empty());
    CHECK_FALSE(fixture.deadline.restartAvailable());
    CHECK(fixture.deadline.hasScheduledDeadline());

    fixture.scheduler.fire(1);
    REQUIRE(fixture.executor.runOne());
    CHECK(fixture.liveElapsedReadCount == 1);
    CHECK(fixture.deadline.restartAvailable());
    CHECK(fixture.availabilityEvents == std::vector{true});
  }

  TEST_CASE("PlaybackRestartDeadline - start pause resume and seek explicitly control the deadline",
            "[runtime][unit][playback-cursor]")
  {
    auto fixture = RestartDeadlineFixture{};
    fixture.deadline.start(Elapsed{500});
    REQUIRE(fixture.scheduler.callCount() == 1);
    CHECK(fixture.scheduler.call(0).deadline == fixture.clock.currentTime + Elapsed{2501});

    fixture.deadline.pause(Elapsed{750});
    CHECK_FALSE(fixture.deadline.isRunning());
    CHECK_FALSE(fixture.deadline.hasScheduledDeadline());
    CHECK(fixture.scheduler.call(0).cancelled);

    fixture.deadline.resume(Elapsed{1000});
    CHECK(fixture.deadline.isRunning());
    CHECK(fixture.deadline.hasScheduledDeadline());
    REQUIRE(fixture.scheduler.callCount() == 2);
    CHECK(fixture.scheduler.call(1).deadline == fixture.clock.currentTime + Elapsed{2001});

    fixture.deadline.seek(Elapsed{3001});
    CHECK(fixture.scheduler.call(1).cancelled);
    CHECK(fixture.deadline.restartAvailable());
    CHECK_FALSE(fixture.deadline.hasScheduledDeadline());
    CHECK(fixture.availabilityEvents == std::vector{true});

    fixture.deadline.seek(Elapsed{3000});
    CHECK_FALSE(fixture.deadline.restartAvailable());
    CHECK(fixture.deadline.hasScheduledDeadline());
    REQUIRE(fixture.scheduler.callCount() == 3);
    CHECK(fixture.scheduler.call(2).deadline == fixture.clock.currentTime + Elapsed{1});
    CHECK(fixture.availabilityEvents == std::vector{true, false});
  }

  TEST_CASE("PlaybackRestartDeadline - current and session replacement cancel obsolete schedules",
            "[runtime][unit][playback-cursor]")
  {
    auto fixture = RestartDeadlineFixture{};
    fixture.deadline.start(Elapsed{0});
    REQUIRE(fixture.scheduler.callCount() == 1);

    fixture.deadline.currentTrackChanged(Elapsed{100}, true);
    CHECK(fixture.scheduler.call(0).cancelled);
    REQUIRE(fixture.scheduler.callCount() == 2);
    CHECK(fixture.scheduler.call(1).deadline == fixture.clock.currentTime + Elapsed{2901});

    fixture.deadline.replaceSession(Elapsed{1500}, true);
    CHECK(fixture.scheduler.call(1).cancelled);
    REQUIRE(fixture.scheduler.callCount() == 3);
    CHECK(fixture.scheduler.call(2).deadline == fixture.clock.currentTime + Elapsed{1501});

    fixture.deadline.replaceSession(Elapsed{500}, false);
    CHECK(fixture.scheduler.call(2).cancelled);
    CHECK(fixture.deadline.isActive());
    CHECK_FALSE(fixture.deadline.isRunning());
    CHECK_FALSE(fixture.deadline.hasScheduledDeadline());

    fixture.deadline.clearSession();
    CHECK_FALSE(fixture.deadline.isActive());
    CHECK_FALSE(fixture.deadline.restartAvailable());
    CHECK(fixture.scheduler.callCount() == 3);
  }

  TEST_CASE("PlaybackRestartDeadline - shutdown suppresses queued work and future controls",
            "[runtime][unit][playback-cursor]")
  {
    auto fixture = RestartDeadlineFixture{};
    fixture.deadline.start(Elapsed{0});
    REQUIRE(fixture.scheduler.callCount() == 1);
    fixture.liveElapsed = Elapsed{3001};
    fixture.scheduler.fire(0);
    REQUIRE(fixture.executor.queuedCount() == 1);

    fixture.deadline.shutdown();
    REQUIRE(fixture.executor.runOne());

    CHECK(fixture.liveElapsedReadCount == 0);
    CHECK(fixture.availabilityEvents.empty());
    CHECK_FALSE(fixture.deadline.isActive());
    CHECK_FALSE(fixture.deadline.isRunning());
    CHECK_FALSE(fixture.deadline.restartAvailable());
    CHECK_FALSE(fixture.deadline.hasScheduledDeadline());

    fixture.deadline.start(Elapsed{3001});
    fixture.deadline.resume(Elapsed{3001});
    fixture.deadline.seek(Elapsed{3001});
    fixture.deadline.currentTrackChanged(Elapsed{3001}, true);
    fixture.deadline.replaceSession(Elapsed{3001}, true);

    CHECK(fixture.scheduler.callCount() == 1);
    CHECK(fixture.availabilityEvents.empty());
  }
} // namespace ao::rt::test
