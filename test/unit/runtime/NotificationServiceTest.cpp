// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include <ao/async/AsyncExceptionHandler.h>
#include <ao/async/Signal.h>
#include <ao/rt/NotificationIds.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    struct NotificationServiceFixture final
    {
      explicit NotificationServiceFixture(async::AsyncExceptionHandler exceptionHandler = {},
                                          NotificationFeedLimits limits = {})
        : runtime{executor, 1, std::move(exceptionHandler)}, service{runtime, limits}
      {
      }

      InlineExecutor executor;
      async::Runtime runtime;
      NotificationService service;
    };
  } // namespace

  TEST_CASE("NotificationService - post publishes one immutable snapshot", "[runtime][unit][notification]")
  {
    auto fixture = NotificationServiceFixture{};
    auto& service = fixture.service;
    auto updates = std::vector<NotificationFeedUpdate>{};
    auto sub = service.onFeedUpdated([&](NotificationFeedUpdate const& update) noexcept { updates.push_back(update); });

    service.post(NotificationSeverity::Info, "test message", NotificationLifetime::history());

    REQUIRE(updates.size() == 1);
    auto const& update = updates.front();
    CHECK(update.mutationKind == NotificationFeedMutationKind::Posted);
    CHECK(update.id == NotificationId{1});
    REQUIRE(update.feedPtr);
    REQUIRE(update.feedPtr->entries.size() == 1);
    CHECK(update.feedPtr->entries.front().id == NotificationId{1});
    CHECK(std::get<std::string>(update.feedPtr->entries.front().message) == "test message");
  }

  TEST_CASE("NotificationService - request post stores product notification state", "[runtime][unit][notification]")
  {
    auto fixture = NotificationServiceFixture{};
    auto const report = NotificationReport{
      .templateId = NotificationReportTemplate::PlaybackDecodeFailed,
      .trackId = TrackId{7},
      .subject = "Song",
      .detail = "bad frame",
    };

    fixture.service.post(NotificationRequest{
      .severity = NotificationSeverity::Error,
      .message = report,
      .lifetime = NotificationLifetime::pinned(),
    });

    auto const feed = fixture.service.feed();
    REQUIRE(feed.entries.size() == 1);
    auto const& entry = feed.entries.front();
    CHECK(entry.id == NotificationId{1});
    CHECK(entry.severity == NotificationSeverity::Error);
    CHECK(entry.lifetime == NotificationLifetime::pinned());
    REQUIRE(std::holds_alternative<NotificationReport>(entry.message));
    CHECK(std::get<NotificationReport>(entry.message) == report);
  }

  TEST_CASE("NotificationService - invalid bounded input publishes nothing and preserves identity",
            "[runtime][unit][notification]")
  {
    auto limits = NotificationFeedLimits{};
    limits.maxTextBytes = 32;
    auto fixture = NotificationServiceFixture{{}, limits};
    auto& service = fixture.service;
    std::int32_t updateCount = 0;
    auto sub = service.onFeedUpdated([&](NotificationFeedUpdate const&) noexcept { ++updateCount; });

    service.post(NotificationSeverity::Info, std::string(33, 'x'), NotificationLifetime::history());
    service.post(NotificationSeverity::Info,
                 "invalid duration",
                 NotificationLifetime::transient(std::chrono::milliseconds::zero()));
    service.createOrUpdate(NotificationReportKey{},
                           NotificationRequest{
                             .message = "empty key",
                             .lifetime = NotificationLifetime::history(),
                           });

    CHECK(service.feed().entries.empty());
    CHECK(updateCount == 0);

    service.post(NotificationSeverity::Info, "accepted", NotificationLifetime::history());

    REQUIRE(service.feed().entries.size() == 1);
    CHECK(service.feed().entries.front().id == NotificationId{1});
  }

  TEST_CASE("NotificationService - structured report bounds cover subject and detail", "[runtime][unit][notification]")
  {
    auto limits = NotificationFeedLimits{};
    limits.maxTextBytes = 32;
    auto fixture = NotificationServiceFixture{{}, limits};
    auto& service = fixture.service;
    std::int32_t updateCount = 0;
    auto sub = service.onFeedUpdated([&](NotificationFeedUpdate const&) noexcept { ++updateCount; });

    service.post(NotificationRequest{
      .message =
        NotificationReport{
          .templateId = NotificationReportTemplate::PlaybackDecodeFailed,
          .subject = std::string(33, 's'),
          .detail = "short",
        },
      .lifetime = NotificationLifetime::history(),
    });
    service.post(NotificationRequest{
      .message =
        NotificationReport{
          .templateId = NotificationReportTemplate::PlaybackDecodeFailed,
          .subject = "short",
          .detail = std::string(33, 'd'),
        },
      .lifetime = NotificationLifetime::history(),
    });

    CHECK(service.feed().entries.empty());
    CHECK(updateCount == 0);

    auto const report = NotificationReport{
      .templateId = NotificationReportTemplate::PlaybackDecodeFailed,
      .trackId = TrackId{7},
      .subject = "Song",
      .detail = "bad frame",
    };
    service.post(NotificationRequest{
      .message = report,
      .lifetime = NotificationLifetime::history(),
    });

    REQUIRE(service.feed().entries.size() == 1);
    CHECK(service.feed().entries.front().id == NotificationId{1});
    REQUIRE(std::holds_alternative<NotificationReport>(service.feed().entries.front().message));
    CHECK(std::get<NotificationReport>(service.feed().entries.front().message) == report);
  }

  TEST_CASE("NotificationService - history eviction is atomic and pinned exhaustion rejects",
            "[runtime][unit][notification]")
  {
    auto limits = NotificationFeedLimits{};
    limits.maxEntries = 3;
    auto fixture = NotificationServiceFixture{{}, limits};
    auto& service = fixture.service;
    auto updates = std::vector<NotificationFeedUpdate>{};
    auto sub = service.onFeedUpdated([&](NotificationFeedUpdate const& update) noexcept { updates.push_back(update); });

    service.post(NotificationSeverity::Info, "first", NotificationLifetime::history());
    service.post(NotificationSeverity::Info, "second", NotificationLifetime::history());
    service.post(NotificationSeverity::Info, "third", NotificationLifetime::history());
    auto const thirdId = updates.back().id;

    REQUIRE(updates.size() == 3);
    CHECK(updates.back().id == thirdId);
    REQUIRE(service.feed().entries.size() == 3);

    service.post(NotificationSeverity::Warning, "pinned one", NotificationLifetime::pinned());
    auto const firstPinnedId = updates.back().id;
    service.post(NotificationSeverity::Warning, "pinned two", NotificationLifetime::pinned());
    auto const secondPinnedId = updates.back().id;
    service.post(NotificationSeverity::Warning, "pinned three", NotificationLifetime::pinned());
    auto const thirdPinnedId = updates.back().id;

    REQUIRE(updates.size() == 6);

    service.post(NotificationSeverity::Error, "no room", NotificationLifetime::pinned());

    CHECK(updates.size() == 6);
    REQUIRE(service.feed().entries.size() == 3);
    CHECK(service.feed().entries[0].id == firstPinnedId);
    CHECK(service.feed().entries[1].id == secondPinnedId);
    CHECK(service.feed().entries[2].id == thirdPinnedId);
  }

  TEST_CASE("NotificationService - keyed report coalesces and suppresses identical updates",
            "[runtime][unit][notification]")
  {
    auto fixture = NotificationServiceFixture{};
    auto& service = fixture.service;
    auto updates = std::vector<NotificationFeedUpdate>{};
    auto sub = service.onFeedUpdated([&](NotificationFeedUpdate const& update) noexcept { updates.push_back(update); });
    auto const key = NotificationReportKey{"playback.skipped.current"};
    auto request = NotificationRequest{
      .severity = NotificationSeverity::Warning,
      .message =
        NotificationReport{
          .templateId = NotificationReportTemplate::PlaybackTracksSkipped,
          .count = 1,
        },
      .lifetime = NotificationLifetime::history(),
    };

    service.createOrUpdate(key, request);
    auto const createdId = service.feed().entries.front().id;
    service.createOrUpdate(key, request);
    std::get<NotificationReport>(request.message).count = 2;
    service.createOrUpdate(key, request);

    REQUIRE(updates.size() == 2);
    CHECK(updates[0].mutationKind == NotificationFeedMutationKind::Posted);
    CHECK(updates[1].mutationKind == NotificationFeedMutationKind::ReportUpdated);
    CHECK(updates[1].id == createdId);
    REQUIRE(service.feed().entries.size() == 1);
    auto const feed = service.feed();
    auto const& entry = feed.entries.front();
    CHECK(entry.id == createdId);
    REQUIRE(entry.optReportKey);
    CHECK(*entry.optReportKey == key);
    REQUIRE(std::holds_alternative<NotificationReport>(entry.message));
    CHECK(std::get<NotificationReport>(entry.message).count == 2);
  }

  TEST_CASE("NotificationService - contract-fulfilling feed observers see committed updates",
            "[runtime][regression][notification][concurrency]")
  {
    // The feed cannot act on an observer failure: publication is already
    // committed. Observers are therefore noexcept; a handler that cannot
    // degrade locally terminates at the throw point, with no later-observer
    // guarantee.
    STATIC_REQUIRE(std::is_nothrow_invocable_v<async::Signal<NotificationFeedUpdate const&>::Handler,
                                               NotificationFeedUpdate const&>);
    STATIC_REQUIRE_FALSE(std::is_constructible_v<async::Signal<NotificationFeedUpdate const&>::Handler,
                                                 decltype([](NotificationFeedUpdate const&) {})>);

    auto fixture = NotificationServiceFixture{};
    auto& service = fixture.service;
    std::int32_t firstObserverCount = 0;
    std::int32_t laterObserverCount = 0;
    auto firstSub = service.onFeedUpdated([&](NotificationFeedUpdate const&) noexcept { ++firstObserverCount; });
    auto laterSub = service.onFeedUpdated([&](NotificationFeedUpdate const&) noexcept { ++laterObserverCount; });

    CHECK_NOTHROW(service.post(NotificationSeverity::Warning, "committed", NotificationLifetime::history()));
    CHECK_NOTHROW(service.post(NotificationSeverity::Info, "later", NotificationLifetime::history()));

    CHECK(firstObserverCount == 2);
    CHECK(laterObserverCount == 2);
  }

  TEST_CASE("NotificationService - reentrant report update preserves immutable publication order",
            "[runtime][regression][notification][concurrency]")
  {
    auto fixture = NotificationServiceFixture{};
    auto& service = fixture.service;
    auto const key = NotificationReportKey{"runtime.operation.current"};
    auto updatedRequest = NotificationRequest{
      .message = "updated",
      .lifetime = NotificationLifetime::history(),
    };
    auto observedFeeds = std::vector<std::shared_ptr<NotificationFeedState const>>{};
    auto observedMessages = std::vector<std::string>{};
    bool observedNullFeed = false;
    auto mutatingSub = service.onFeedUpdated(
      [&](NotificationFeedUpdate const& update) noexcept
      {
        if (update.mutationKind == NotificationFeedMutationKind::Posted)
        {
          service.createOrUpdate(key, updatedRequest);
        }
      });
    auto observingSub = service.onFeedUpdated(
      [&](NotificationFeedUpdate const& update) noexcept
      {
        if (!update.feedPtr)
        {
          observedNullFeed = true;
          return;
        }

        if (update.feedPtr->entries.size() != 1)
        {
          return;
        }

        observedFeeds.push_back(update.feedPtr);
        observedMessages.emplace_back(std::get<std::string>(update.feedPtr->entries.front().message));
      });

    service.createOrUpdate(key,
                           NotificationRequest{
                             .message = "initial",
                             .lifetime = NotificationLifetime::history(),
                           });

    CHECK_FALSE(observedNullFeed);
    REQUIRE(observedFeeds.size() == 2);
    CHECK(observedMessages == std::vector<std::string>{"initial", "updated"});
    CHECK(std::get<std::string>(observedFeeds[0]->entries.front().message) == "initial");
    CHECK(std::get<std::string>(observedFeeds[1]->entries.front().message) == "updated");
    CHECK(std::get<std::string>(service.feed().entries.front().message) == "updated");
  }

  TEST_CASE("NotificationService - reentrant post advances the id watermark before publication",
            "[runtime][regression][notification][concurrency]")
  {
    auto fixture = NotificationServiceFixture{};
    auto& service = fixture.service;
    bool nestedPosted = false;
    auto sub = service.onFeedUpdated(
      [&](NotificationFeedUpdate const&) noexcept
      {
        if (!nestedPosted)
        {
          nestedPosted = true;
          service.post(NotificationSeverity::Info, "nested", NotificationLifetime::history());
        }
      });

    service.post(NotificationSeverity::Info, "outer", NotificationLifetime::history());

    REQUIRE(service.feed().entries.size() == 2);
    CHECK(service.feed().entries[0].id == NotificationId{1});
    CHECK(service.feed().entries[1].id == NotificationId{2});
    CHECK(std::get<std::string>(service.feed().entries[0].message) == "outer");
    CHECK(std::get<std::string>(service.feed().entries[1].message) == "nested");
  }
} // namespace ao::rt::test
