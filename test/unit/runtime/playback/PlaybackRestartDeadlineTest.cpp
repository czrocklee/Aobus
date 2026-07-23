// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/playback/PlaybackRestartDeadline.h"

#include "test/unit/RuntimeTestSupport.h"
#include <ao/async/Runtime.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    using Elapsed = PlaybackRestartDeadline::Elapsed;

    Elapsed zeroElapsed()
    {
      return Elapsed{0};
    }

    struct RestartDeadlineFixture final
    {
      RestartDeadlineFixture()
        : asyncRuntime{executor, 1, {}, &scheduler}
        , deadline{asyncRuntime,
                   [this]
                   {
                     ++liveElapsedReadCount;
                     return liveElapsed;
                   },
                   [this](bool const available) { availabilityEvents.push_back(available); }}
      {
      }

      // These fixture values are intentionally public as the tests' assertion surface.
      ManualExecutor executor;
      // The sleeper is declared before asyncRuntime so it outlives the Runtime that borrows it.
      ControlledSleeper scheduler;
      async::Runtime asyncRuntime;
      Elapsed liveElapsed{0};
      std::size_t liveElapsedReadCount = 0;
      std::vector<bool> availabilityEvents;
      PlaybackRestartDeadline deadline;
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
    REQUIRE(fixture.scheduler.waitForCallCount(1));
    CHECK(fixture.scheduler.call(0).delay == Elapsed{2});

    fixture.liveElapsed = Elapsed{3000};
    REQUIRE(fixture.scheduler.fire(0));

    fixture.executor.checkQueued();
    CHECK(fixture.executor.queuedCount() == 1);
    CHECK(fixture.liveElapsedReadCount == 0);
    CHECK(fixture.availabilityEvents.empty());
    REQUIRE(fixture.executor.runOne());
    CHECK(fixture.liveElapsedReadCount == 1);
    CHECK_FALSE(fixture.deadline.restartAvailable());
    REQUIRE(fixture.scheduler.waitForCallCount(2));
    CHECK(fixture.scheduler.call(1).delay == Elapsed{1});

    fixture.liveElapsed = Elapsed{3001};
    REQUIRE(fixture.scheduler.fire(1));
    CHECK_FALSE(fixture.deadline.restartAvailable());
    fixture.executor.checkQueued();
    CHECK(fixture.executor.queuedCount() == 1);

    REQUIRE(fixture.executor.runOne());
    CHECK(fixture.deadline.restartAvailable());
    CHECK_FALSE(fixture.deadline.hasScheduledDeadline());
    CHECK(fixture.availabilityEvents == std::vector{true});
  }

  TEST_CASE("PlaybackRestartDeadline - cancellation suppresses a queued callback from an older deadline",
            "[runtime][unit][playback-cursor]")
  {
    auto fixture = RestartDeadlineFixture{};
    fixture.deadline.start(Elapsed{0});
    REQUIRE(fixture.scheduler.waitForCallCount(1));
    REQUIRE(fixture.scheduler.fire(0));
    fixture.executor.checkQueued();
    REQUIRE(fixture.executor.queuedCount() == 1);

    fixture.deadline.seek(Elapsed{1000});
    REQUIRE(fixture.scheduler.waitForCallCount(2));
    fixture.liveElapsed = Elapsed{3001};

    REQUIRE(fixture.executor.runOne());
    CHECK(fixture.liveElapsedReadCount == 0);
    CHECK(fixture.availabilityEvents.empty());
    CHECK_FALSE(fixture.deadline.restartAvailable());
    CHECK(fixture.deadline.hasScheduledDeadline());

    REQUIRE(fixture.scheduler.fire(1));
    fixture.executor.checkQueued();
    REQUIRE(fixture.executor.runOne());
    CHECK(fixture.liveElapsedReadCount == 1);
    CHECK(fixture.deadline.restartAvailable());
    CHECK(fixture.availabilityEvents == std::vector{true});
  }

  TEST_CASE("PlaybackRestartDeadline - reentrant availability changes install only the replacement deadline",
            "[runtime][regression][playback-cursor][concurrency]")
  {
    auto executor = ManualExecutor{};
    auto scheduler = ControlledSleeper{};
    auto asyncRuntime = async::Runtime{executor, 1, {}, &scheduler};
    auto availabilityEvents = std::vector<bool>{};
    auto deadlinePtr = std::unique_ptr<PlaybackRestartDeadline>{};
    deadlinePtr = std::make_unique<PlaybackRestartDeadline>(asyncRuntime,
                                                            zeroElapsed,
                                                            [&](bool const available)
                                                            {
                                                              availabilityEvents.push_back(available);

                                                              if (available)
                                                              {
                                                                deadlinePtr->seek(Elapsed{0});
                                                              }
                                                            });

    deadlinePtr->start(Elapsed{3001});

    CHECK(availabilityEvents == std::vector{true, false});
    CHECK_FALSE(deadlinePtr->restartAvailable());
    CHECK(deadlinePtr->hasScheduledDeadline());
    REQUIRE(scheduler.waitForCallCount(1));
    CHECK(scheduler.callCount() == 1);
    CHECK(scheduler.call(0).delay == Elapsed{3001});
  }

  TEST_CASE("PlaybackRestartDeadline - start pause resume and seek explicitly control the deadline",
            "[runtime][unit][playback-cursor]")
  {
    auto fixture = RestartDeadlineFixture{};
    fixture.deadline.start(Elapsed{500});
    REQUIRE(fixture.scheduler.waitForCallCount(1));
    CHECK(fixture.scheduler.call(0).delay == Elapsed{2501});

    fixture.deadline.pause(Elapsed{750});
    CHECK_FALSE(fixture.deadline.isRunning());
    CHECK_FALSE(fixture.deadline.hasScheduledDeadline());
    REQUIRE(fixture.scheduler.waitForCancellation(0));
    CHECK(fixture.scheduler.call(0).cancelled);

    fixture.deadline.resume(Elapsed{1000});
    CHECK(fixture.deadline.isRunning());
    CHECK(fixture.deadline.hasScheduledDeadline());
    REQUIRE(fixture.scheduler.waitForCallCount(2));
    CHECK(fixture.scheduler.call(1).delay == Elapsed{2001});

    fixture.deadline.seek(Elapsed{3001});
    REQUIRE(fixture.scheduler.waitForCancellation(1));
    CHECK(fixture.scheduler.call(1).cancelled);
    CHECK(fixture.deadline.restartAvailable());
    CHECK_FALSE(fixture.deadline.hasScheduledDeadline());
    CHECK(fixture.availabilityEvents == std::vector{true});

    fixture.deadline.seek(Elapsed{3000});
    CHECK_FALSE(fixture.deadline.restartAvailable());
    CHECK(fixture.deadline.hasScheduledDeadline());
    REQUIRE(fixture.scheduler.waitForCallCount(3));
    CHECK(fixture.scheduler.call(2).delay == Elapsed{1});
    CHECK(fixture.availabilityEvents == std::vector{true, false});
  }

  TEST_CASE("PlaybackRestartDeadline - current and session replacement cancel obsolete schedules",
            "[runtime][unit][playback-cursor]")
  {
    auto fixture = RestartDeadlineFixture{};
    fixture.deadline.start(Elapsed{0});
    REQUIRE(fixture.scheduler.waitForCallCount(1));

    fixture.deadline.currentTrackChanged(Elapsed{100}, true);
    REQUIRE(fixture.scheduler.waitForCancellation(0));
    CHECK(fixture.scheduler.call(0).cancelled);
    REQUIRE(fixture.scheduler.waitForCallCount(2));
    CHECK(fixture.scheduler.call(1).delay == Elapsed{2901});

    fixture.deadline.replaceSession(Elapsed{1500}, true);
    REQUIRE(fixture.scheduler.waitForCancellation(1));
    CHECK(fixture.scheduler.call(1).cancelled);
    REQUIRE(fixture.scheduler.waitForCallCount(3));
    CHECK(fixture.scheduler.call(2).delay == Elapsed{1501});

    fixture.deadline.replaceSession(Elapsed{500}, false);
    REQUIRE(fixture.scheduler.waitForCancellation(2));
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
    REQUIRE(fixture.scheduler.waitForCallCount(1));
    fixture.liveElapsed = Elapsed{3001};
    REQUIRE(fixture.scheduler.fire(0));
    fixture.executor.checkQueued();
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
