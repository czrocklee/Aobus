// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include <ao/rt/NotificationIds.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/Subscription.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    struct NotificationExpiryFixture final
    {
      NotificationExpiryFixture()
        : runtime{executor, 1, exceptions.handler(), &sleeper}
        , service{runtime}
        , updateSub{service.onFeedUpdated([this](NotificationFeedUpdate const& update) { updates.push_back(update); })}
      {
      }

      ControlledSleeper sleeper;
      QueuedExecutor executor;
      AsyncExceptionRecorder exceptions;
      async::Runtime runtime;
      NotificationService service;
      std::vector<NotificationFeedUpdate> updates;
      Subscription updateSub;
    };
  } // namespace

  TEST_CASE("NotificationService expiry - transient lifetime expires through the callback executor",
            "[runtime][regression][notification][concurrency]")
  {
    auto fixture = NotificationExpiryFixture{};
    constexpr auto kDuration = std::chrono::milliseconds{1250};
    auto const id =
      fixture.service.post(NotificationSeverity::Info, "Temporary", NotificationLifetime::transient(kDuration));

    REQUIRE(fixture.sleeper.waitForCallCount(1));
    CHECK(fixture.sleeper.call(0).delay == kDuration);
    REQUIRE(fixture.service.feed().entries.size() == 1);
    CHECK(fixture.service.feed().entries.front().lifetimeGeneration == 1);

    REQUIRE(fixture.sleeper.fire(0));
    fixture.executor.checkQueued();

    CHECK(fixture.service.feed().revision == 1);
    CHECK(fixture.service.feed().entries.size() == 1);

    fixture.executor.drain();

    CHECK(fixture.service.feed().revision == 2);
    CHECK(fixture.service.feed().entries.empty());
    REQUIRE(fixture.updates.size() == 2);
    CHECK(fixture.updates.back().mutationKind == NotificationFeedMutationKind::Expired);
    CHECK(fixture.updates.back().affectedIds == std::vector{id});
    CHECK(fixture.exceptions.snapshot().empty());
  }

  TEST_CASE("NotificationService expiry - retained lifetimes do not schedule expiry", "[runtime][unit][notification]")
  {
    auto fixture = NotificationExpiryFixture{};

    fixture.service.post(NotificationSeverity::Warning, "History", NotificationLifetime::sessionHistory());
    fixture.service.post(NotificationSeverity::Error, "Pinned", NotificationLifetime::untilDismissed());

    CHECK(fixture.sleeper.callCount() == 0);
    REQUIRE(fixture.service.feed().entries.size() == 2);
    CHECK(fixture.service.feed().entries[0].lifetimeGeneration == 0);
    CHECK(fixture.service.feed().entries[1].lifetimeGeneration == 0);
  }

  TEST_CASE("NotificationService expiry - update generation rejects an already queued timer",
            "[runtime][regression][notification][concurrency]")
  {
    auto fixture = NotificationExpiryFixture{};
    auto const id = fixture.service.post(
      NotificationSeverity::Info, "Initial", NotificationLifetime::transient(std::chrono::seconds{30}));
    REQUIRE(fixture.sleeper.waitForCallCount(1));

    REQUIRE(fixture.sleeper.fire(0));
    fixture.executor.checkQueued();
    REQUIRE(fixture.service.updateMessage(id, "Updated"));
    REQUIRE(fixture.sleeper.waitForCallCount(2));

    auto feed = fixture.service.feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK(feed.entries.front().message == "Updated");
    CHECK(feed.entries.front().lifetimeGeneration == 2);

    fixture.executor.drain();

    feed = fixture.service.feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK(feed.entries.front().message == "Updated");
    CHECK(feed.revision == 2);

    REQUIRE(fixture.sleeper.fire(1));
    fixture.executor.checkQueued();
    fixture.executor.drain();

    CHECK(fixture.service.feed().entries.empty());
    CHECK(fixture.service.feed().revision == 3);
    REQUIRE(fixture.updates.size() == 3);
    CHECK(fixture.updates.back().mutationKind == NotificationFeedMutationKind::Expired);
  }

  TEST_CASE("NotificationService expiry - explicit dismissal wins over queued expiry",
            "[runtime][regression][notification][concurrency]")
  {
    auto fixture = NotificationExpiryFixture{};
    auto const id = fixture.service.post(
      NotificationSeverity::Info, "Temporary", NotificationLifetime::transient(std::chrono::seconds{30}));
    REQUIRE(fixture.sleeper.waitForCallCount(1));
    REQUIRE(fixture.sleeper.fire(0));
    fixture.executor.checkQueued();

    fixture.service.dismiss(id);
    CHECK(fixture.service.feed().revision == 2);

    fixture.executor.drain();

    CHECK(fixture.service.feed().revision == 2);
    CHECK(fixture.service.feed().entries.empty());
    REQUIRE(fixture.updates.size() == 2);
    CHECK(fixture.updates.back().mutationKind == NotificationFeedMutationKind::Dismissed);
  }

  TEST_CASE("NotificationService expiry - dismissal cancels a pending timer",
            "[runtime][regression][notification][concurrency]")
  {
    auto fixture = NotificationExpiryFixture{};
    auto const id = fixture.service.post(
      NotificationSeverity::Info, "Temporary", NotificationLifetime::transient(std::chrono::seconds{30}));
    REQUIRE(fixture.sleeper.waitForCallCount(1));

    fixture.service.dismiss(id);

    REQUIRE(fixture.sleeper.waitForCancellation(0));
    CHECK_FALSE(fixture.sleeper.fire(0));
    CHECK(fixture.service.feed().entries.empty());
    CHECK(fixture.service.feed().revision == 2);
  }

  TEST_CASE("NotificationService expiry - queued callback is safe after service destruction",
            "[runtime][regression][notification][concurrency]")
  {
    auto sleeper = ControlledSleeper{};
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor, 1, {}, &sleeper};

    {
      auto service = NotificationService{runtime};
      service.post(NotificationSeverity::Info, "Temporary", NotificationLifetime::transient(std::chrono::seconds{30}));
      REQUIRE(sleeper.waitForCallCount(1));
      REQUIRE(sleeper.fire(0));
      executor.checkQueued();
    }

    CHECK_NOTHROW(executor.drain());
  }
} // namespace ao::rt::test
