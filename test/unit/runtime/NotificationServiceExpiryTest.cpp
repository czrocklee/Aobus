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
    fixture.service.post(NotificationSeverity::Info, "Temporary", NotificationLifetime::transient(kDuration));
    REQUIRE(fixture.service.feed().entries.size() == 1);
    auto const id = fixture.service.feed().entries.front().id;

    REQUIRE(fixture.sleeper.waitForCallCount(1));
    CHECK(fixture.sleeper.call(0).delay == kDuration);
    REQUIRE(fixture.service.feed().entries.size() == 1);

    REQUIRE(fixture.sleeper.fire(0));
    fixture.executor.checkQueued();

    CHECK(fixture.service.feed().entries.size() == 1);

    fixture.executor.drain();

    CHECK(fixture.service.feed().entries.empty());
    REQUIRE(fixture.updates.size() == 2);
    CHECK(fixture.updates.back().mutationKind == NotificationFeedMutationKind::Expired);
    CHECK(fixture.updates.back().id == id);
    CHECK(fixture.exceptions.snapshot().empty());
  }

  TEST_CASE("NotificationService expiry - retained lifetimes do not schedule expiry", "[runtime][unit][notification]")
  {
    auto fixture = NotificationExpiryFixture{};

    fixture.service.post(NotificationSeverity::Warning, "History", NotificationLifetime::history());
    fixture.service.post(NotificationSeverity::Error, "Pinned", NotificationLifetime::pinned());

    CHECK(fixture.sleeper.callCount() == 0);
    REQUIRE(fixture.service.feed().entries.size() == 2);
  }

  TEST_CASE("NotificationService expiry - keyed update generation rejects an already queued timer",
            "[runtime][regression][notification][concurrency]")
  {
    auto fixture = NotificationExpiryFixture{};
    auto const key = NotificationReportKey{"runtime.operation.current"};
    auto request = NotificationRequest{
      .message = "Initial",
      .lifetime = NotificationLifetime::transient(std::chrono::seconds{30}),
    };
    fixture.service.createOrUpdate(key, request);
    REQUIRE(fixture.service.feed().entries.size() == 1);
    REQUIRE(fixture.sleeper.waitForCallCount(1));

    REQUIRE(fixture.sleeper.fire(0));
    fixture.executor.checkQueued();
    request.message = "Updated";
    fixture.service.createOrUpdate(key, request);
    REQUIRE(fixture.sleeper.waitForCallCount(2));

    auto feed = fixture.service.feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK(std::get<std::string>(feed.entries.front().message) == "Updated");

    fixture.executor.drain();

    feed = fixture.service.feed();
    REQUIRE(feed.entries.size() == 1);
    CHECK(std::get<std::string>(feed.entries.front().message) == "Updated");

    REQUIRE(fixture.sleeper.fire(1));
    fixture.executor.checkQueued();
    fixture.executor.drain();

    CHECK(fixture.service.feed().entries.empty());
    REQUIRE(fixture.updates.size() == 3);
    CHECK(fixture.updates.back().mutationKind == NotificationFeedMutationKind::Expired);
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

    fixture.service.createOrUpdate(key, request);
    REQUIRE(fixture.service.feed().entries.size() == 1);
    auto const createdId = fixture.service.feed().entries.front().id;
    REQUIRE(fixture.sleeper.waitForCallCount(1));

    fixture.service.createOrUpdate(key, request);
    CHECK(fixture.sleeper.callCount() == 1);
    CHECK(fixture.updates.size() == 1);
    CHECK(fixture.service.feed().entries.front().id == createdId);

    REQUIRE(fixture.sleeper.fire(0));
    fixture.executor.checkQueued();

    request.lifetime = NotificationLifetime::history();
    fixture.service.createOrUpdate(key, request);
    CHECK(fixture.updates.size() == 2);

    request.lifetime = NotificationLifetime::transient(std::chrono::seconds{45});
    fixture.service.createOrUpdate(key, request);
    CHECK(fixture.service.feed().entries.front().id == createdId);
    REQUIRE(fixture.sleeper.waitForCallCount(2));
    CHECK(fixture.sleeper.call(1).delay == std::chrono::seconds{45});
    CHECK(fixture.updates.size() == 3);

    fixture.executor.drain();

    REQUIRE(fixture.service.feed().entries.size() == 1);
    CHECK(fixture.service.feed().entries.front().id == createdId);
    CHECK(fixture.updates.size() == 3);

    REQUIRE(fixture.sleeper.fire(1));
    fixture.executor.checkQueued();
    fixture.executor.drain();

    CHECK(fixture.service.feed().entries.empty());
    CHECK(fixture.updates.size() == 4);
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
