// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/runtime/AsyncTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include <ao/async/Runtime.h>
#include <ao/async/Subscription.h>
#include <ao/rt/NotificationIds.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    struct NotificationExpiryFixture final
    {
      NotificationExpiryFixture()
        : runtime{executor, 1, &sleeper}
        , service{runtime}
        , updateSub{
            service.onFeedUpdated([this](NotificationFeedUpdate const& update) noexcept { updates.push_back(update); })}
      {
      }

      ControlledSleeper sleeper;
      QueuedExecutor executor;
      async::Runtime runtime;
      NotificationService service;
      std::vector<NotificationFeedUpdate> updates;
      async::Subscription updateSub;
    };
  } // namespace

  TEST_CASE("NotificationService expiry - transient lifetime expires through the callback executor",
            "[runtime][unit][notification][concurrency]")
  {
    auto fixture = NotificationExpiryFixture{};
    constexpr auto kDuration = std::chrono::milliseconds{1250};
    fixture.service.post(NotificationSeverity::Info, "Temporary", NotificationLifetime::transient(kDuration));
    REQUIRE(fixture.service.feed().entries.size() == 1);
    auto const id = fixture.service.feed().entries.front().id;

    REQUIRE(fixture.sleeper.tryWaitForCallCount(1));
    CHECK(fixture.sleeper.call(0).delay == kDuration);
    REQUIRE(fixture.service.feed().entries.size() == 1);

    REQUIRE(fixture.sleeper.tryFire(0));
    fixture.executor.checkQueued();

    CHECK(fixture.service.feed().entries.size() == 1);

    fixture.executor.drain();

    CHECK(fixture.service.feed().entries.empty());
    REQUIRE(fixture.updates.size() == 2);
    CHECK(fixture.updates.back().mutationKind == NotificationFeedMutationKind::Expired);
    CHECK(fixture.updates.back().id == id);
  }

  TEST_CASE("NotificationService expiry - retained lifetimes do not schedule expiry", "[runtime][unit][notification]")
  {
    auto fixture = NotificationExpiryFixture{};

    fixture.service.post(NotificationSeverity::Warning, "History", NotificationLifetime::history());
    fixture.service.post(NotificationSeverity::Error, "Pinned", NotificationLifetime::pinned());

    REQUIRE(fixture.service.feed().entries.size() == 2);
    // The single worker reaches this later timer after any erroneous retained expiry.
    constexpr auto kControlDuration = std::chrono::milliseconds{1750};
    fixture.service.post(NotificationSeverity::Info, "Control", NotificationLifetime::transient(kControlDuration));
    REQUIRE(fixture.sleeper.tryWaitForCallCount(1));
    REQUIRE(fixture.sleeper.callCount() == 1);
    CHECK(fixture.sleeper.call(0).delay == kControlDuration);
    REQUIRE(fixture.sleeper.tryFire(0));
    fixture.executor.checkQueued();
    fixture.executor.drain();
    CHECK(fixture.sleeper.callCount() == 1);
    REQUIRE(fixture.service.feed().entries.size() == 2);
    CHECK(std::get<std::string>(fixture.service.feed().entries[0].message) == "History");
    CHECK(std::get<std::string>(fixture.service.feed().entries[1].message) == "Pinned");
  }

  TEST_CASE("NotificationService expiry - keyed retention cancels a still-active timer",
            "[runtime][unit][notification][concurrency]")
  {
    auto fixture = NotificationExpiryFixture{};
    auto const key = NotificationReportKey{"runtime.operation.current"};
    auto request =
      NotificationRequest{.message = "Retained", .lifetime = NotificationLifetime::transient(std::chrono::seconds{30})};
    fixture.service.createOrUpdate(key, request);
    REQUIRE(fixture.service.feed().entries.size() == 1);
    auto const id = fixture.service.feed().entries.front().id;
    REQUIRE(fixture.sleeper.tryWaitForCallCount(1));
    request.lifetime = NotificationLifetime::history();

    fixture.service.createOrUpdate(key, request);

    REQUIRE(fixture.sleeper.tryWaitForCancellation(0));
    CHECK_FALSE(fixture.sleeper.tryFire(0));
    fixture.executor.drain();
    REQUIRE(fixture.service.feed().entries.size() == 1);
    CHECK(fixture.service.feed().entries.front().id == id);
    CHECK(fixture.service.feed().entries.front().lifetime == NotificationLifetime::history());
    REQUIRE(fixture.updates.size() == 2);
    CHECK(fixture.updates[0].mutationKind == NotificationFeedMutationKind::Posted);
    CHECK(fixture.updates[1].mutationKind == NotificationFeedMutationKind::ReportUpdated);
    CHECK(fixture.updates[1].id == id);
  }

  TEST_CASE("NotificationService expiry - keyed update registration identity rejects an already queued timer",
            "[runtime][unit][notification][concurrency]")
  {
    auto fixture = NotificationExpiryFixture{};
    auto const key = NotificationReportKey{"runtime.operation.current"};
    auto request = NotificationRequest{
      .message = "Initial",
      .lifetime = NotificationLifetime::transient(std::chrono::seconds{30}),
    };
    fixture.service.createOrUpdate(key, request);
    REQUIRE(fixture.service.feed().entries.size() == 1);
    REQUIRE(fixture.sleeper.tryWaitForCallCount(1));

    REQUIRE(fixture.sleeper.tryFire(0));
    fixture.executor.checkQueued();
    request.message = "Updated";
    fixture.service.createOrUpdate(key, request);
    REQUIRE(fixture.sleeper.tryWaitForCallCount(2));

    auto feed = fixture.service.feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK(std::get<std::string>(feed.entries.front().message) == "Updated");

    fixture.executor.drain();

    feed = fixture.service.feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK(std::get<std::string>(feed.entries.front().message) == "Updated");

    REQUIRE(fixture.sleeper.tryFire(1));
    fixture.executor.checkQueued();
    fixture.executor.drain();

    CHECK(fixture.service.feed().entries.empty());
    REQUIRE(fixture.updates.size() == 3);
    CHECK(fixture.updates.back().mutationKind == NotificationFeedMutationKind::Expired);
  }

  TEST_CASE("NotificationService expiry - keyed lifetime transitions reject a queued obsolete timer",
            "[runtime][unit][notification][concurrency]")
  {
    auto fixture = NotificationExpiryFixture{};
    auto const key = NotificationReportKey{"runtime.operation.current"};
    auto request = NotificationRequest{
      .severity = NotificationSeverity::Info,
      .message = "Working",
      .lifetime = NotificationLifetime::transient(std::chrono::seconds{30}),
    };

    fixture.service.createOrUpdate(key, request);
    REQUIRE(fixture.service.feed().entries.size() == 1);
    auto const createdId = fixture.service.feed().entries.front().id;
    REQUIRE(fixture.sleeper.tryWaitForCallCount(1));

    fixture.service.createOrUpdate(key, request);
    CHECK(fixture.sleeper.callCount() == 1);
    CHECK(fixture.updates.size() == 1);
    CHECK(fixture.service.feed().entries.front().id == createdId);

    REQUIRE(fixture.sleeper.tryFire(0));
    fixture.executor.checkQueued();

    request.lifetime = NotificationLifetime::history();
    fixture.service.createOrUpdate(key, request);
    CHECK(fixture.updates.size() == 2);

    request.lifetime = NotificationLifetime::transient(std::chrono::seconds{45});
    fixture.service.createOrUpdate(key, request);
    CHECK(fixture.service.feed().entries.front().id == createdId);
    REQUIRE(fixture.sleeper.tryWaitForCallCount(2));
    CHECK(fixture.sleeper.call(1).delay == std::chrono::seconds{45});
    CHECK(fixture.updates.size() == 3);

    fixture.executor.drain();

    REQUIRE(fixture.service.feed().entries.size() == 1);
    CHECK(fixture.service.feed().entries.front().id == createdId);
    CHECK(fixture.updates.size() == 3);

    REQUIRE(fixture.sleeper.tryFire(1));
    fixture.executor.checkQueued();
    fixture.executor.drain();

    CHECK(fixture.service.feed().entries.empty());
    REQUIRE(fixture.updates.size() == 4);
    CHECK(fixture.updates.back().mutationKind == NotificationFeedMutationKind::Expired);
    CHECK(fixture.updates.back().id == createdId);
  }

  TEST_CASE("NotificationService expiry - queued callback is safe after service destruction",
            "[runtime][unit][notification][concurrency]")
  {
    auto sleeper = ControlledSleeper{};
    auto executor = QueuedExecutor{};
    auto runtime = async::Runtime{executor, 1, &sleeper};

    {
      auto service = NotificationService{runtime};
      service.post(NotificationSeverity::Info, "Temporary", NotificationLifetime::transient(std::chrono::seconds{30}));
      REQUIRE(sleeper.tryWaitForCallCount(1));
      REQUIRE(sleeper.tryFire(0));
      executor.checkQueued();
    }

    CHECK_NOTHROW(executor.drain());
  }
} // namespace ao::rt::test
