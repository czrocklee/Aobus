// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include <ao/async/Subscription.h>
#include <ao/rt/NotificationIds.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>

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
      async::Subscription updateSub;
    };
  } // namespace

  TEST_CASE("NotificationService expiry - transient lifetime expires through the callback executor",
            "[runtime][regression][notification][concurrency]")
  {
    auto fixture = NotificationExpiryFixture{};
    constexpr auto kDuration = std::chrono::milliseconds{1250};
    auto const id =
      fixture.service.post(NotificationSeverity::Info, "Temporary", NotificationLifetime::transient(kDuration)).id;

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
    auto const id =
      fixture.service
        .post(NotificationSeverity::Info, "Initial", NotificationLifetime::transient(std::chrono::seconds{30}))
        .id;
    REQUIRE(fixture.sleeper.waitForCallCount(1));

    REQUIRE(fixture.sleeper.fire(0));
    fixture.executor.checkQueued();
    REQUIRE(fixture.service.updateMessage(id, "Updated").outcome == NotificationMutationOutcome::Applied);
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
    auto const id =
      fixture.service
        .post(NotificationSeverity::Info, "Temporary", NotificationLifetime::transient(std::chrono::seconds{30}))
        .id;
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
    auto const id =
      fixture.service
        .post(NotificationSeverity::Info, "Temporary", NotificationLifetime::transient(std::chrono::seconds{30}))
        .id;
    REQUIRE(fixture.sleeper.waitForCallCount(1));

    fixture.service.dismiss(id);

    REQUIRE(fixture.sleeper.waitForCancellation(0));
    CHECK_FALSE(fixture.sleeper.fire(0));
    CHECK(fixture.service.feed().entries.empty());
    CHECK(fixture.service.feed().revision == 2);
  }

  TEST_CASE("NotificationService expiry - keyed lifetime transitions reject a queued obsolete timer",
            "[runtime][regression][notification][concurrency]")
  {
    auto fixture = NotificationExpiryFixture{};
    auto const key = NotificationReportKey{"runtime.operation.current"};
    auto request = NotificationRequest{
      .severity = NotificationSeverity::Info,
      .message = "Working",
      .lifetime = NotificationLifetime::transient(std::chrono::seconds{30}),
    };

    auto const created = fixture.service.createOrUpdate(key, request);
    REQUIRE(created.outcome == NotificationMutationOutcome::Applied);
    REQUIRE(fixture.sleeper.waitForCallCount(1));

    auto const unchanged = fixture.service.createOrUpdate(key, request);
    CHECK(unchanged.outcome == NotificationMutationOutcome::Unchanged);
    CHECK(unchanged.id == created.id);
    CHECK(fixture.sleeper.callCount() == 1);
    CHECK(fixture.service.feed().revision == 1);
    CHECK(fixture.service.feed().entries.front().lifetimeGeneration == 1);

    REQUIRE(fixture.sleeper.fire(0));
    fixture.executor.checkQueued();

    request.lifetime = NotificationLifetime::sessionHistory();
    auto const retained = fixture.service.createOrUpdate(key, request);
    CHECK(retained.outcome == NotificationMutationOutcome::Applied);
    CHECK(fixture.service.feed().revision == 2);
    CHECK(fixture.service.feed().entries.front().lifetimeGeneration == 1);

    request.lifetime = NotificationLifetime::transient(std::chrono::seconds{45});
    auto const restarted = fixture.service.createOrUpdate(key, request);
    CHECK(restarted.outcome == NotificationMutationOutcome::Applied);
    CHECK(restarted.id == created.id);
    REQUIRE(fixture.sleeper.waitForCallCount(2));
    CHECK(fixture.sleeper.call(1).delay == std::chrono::seconds{45});
    CHECK(fixture.service.feed().revision == 3);
    CHECK(fixture.service.feed().entries.front().lifetimeGeneration == 2);

    fixture.executor.drain();

    REQUIRE(fixture.service.feed().entries.size() == 1);
    CHECK(fixture.service.feed().entries.front().id == created.id);
    CHECK(fixture.service.feed().revision == 3);

    REQUIRE(fixture.sleeper.fire(1));
    fixture.executor.checkQueued();
    fixture.executor.drain();

    CHECK(fixture.service.feed().entries.empty());
    CHECK(fixture.service.feed().revision == 4);
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
