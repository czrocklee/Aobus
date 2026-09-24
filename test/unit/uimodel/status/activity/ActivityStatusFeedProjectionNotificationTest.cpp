// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/uimodel/status/activity/ActivityStatusFeedProjectionTestSupport.h"
#include "uimodel/status/activity/ActivityStatusFeedProjection.h"
#include <ao/rt/NotificationState.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>

namespace ao::uimodel::test
{
  TEST_CASE("ActivityStatusFeedProjection - compact groups choose the highest severity",
            "[uimodel][unit][activity-status]")
  {
    auto currentFeed = feed({entry(rt::NotificationId{2}, rt::NotificationSeverity::Warning, "Warn A"),
                             entry(rt::NotificationId{3}, rt::NotificationSeverity::Error, "Error A"),
                             entry(rt::NotificationId{4}, rt::NotificationSeverity::Error, "Error B")});
    auto severityProjection = ActivityStatusFeedProjection{ao::test::englishMessageCatalog(), currentFeed};

    auto const& compact = severityProjection.viewState().compact;
    CHECK(compact.kind == ActivityStatusKind::Error);
    CHECK(compact.text == "2 errors");
    CHECK(compact.hasDetails);
  }

  TEST_CASE("ActivityStatusFeedProjection - compact groups use the selected locale", "[uimodel][unit][activity-status]")
  {
    auto currentFeed = feed({entry(rt::NotificationId{30}, rt::NotificationSeverity::Warning, "Warn A"),
                             entry(rt::NotificationId{31}, rt::NotificationSeverity::Warning, "Warn B")});
    auto germanProjection = ActivityStatusFeedProjection{ao::test::messageCatalog("de-DE"), currentFeed};
    CHECK(germanProjection.viewState().compact.text == "2 Warnungen");

    auto pseudoProjection = ActivityStatusFeedProjection{ao::test::messageCatalog("qps-ploc"), currentFeed};
    CHECK(pseudoProjection.viewState().compact.text == "[!! 2 wààrñïïñgs !!]");
  }

  TEST_CASE("ActivityStatusFeedProjection - runtime notification lifetime remains authoritative",
            "[uimodel][unit][activity-status]")
  {
    auto feedProjection = ActivityStatusFeedProjection{ao::test::englishMessageCatalog(), feed({})};

    SECTION("runtime-transient notification does not create a presentation-local timeout")
    {
      auto currentFeed = feed({entry(rt::NotificationId{5},
                                     rt::NotificationSeverity::Info,
                                     "Saved playlist",
                                     rt::NotificationLifetime::transient(std::chrono::milliseconds{1500}))});

      feedProjection.handleFeedUpdated(postedUpdate(currentFeed, rt::NotificationId{5}));

      auto const& compact = feedProjection.viewState().compact;
      CHECK(compact.kind == ActivityStatusKind::Info);
      CHECK(compact.text == "Saved playlist");
      CHECK_FALSE(compact.optAutoDismissTimeout);
    }

    SECTION("notification-derived transient disappears when its source leaves the feed")
    {
      auto currentFeed = feed({entry(rt::NotificationId{18}, rt::NotificationSeverity::Info, "Saved playlist")});
      feedProjection.handleFeedUpdated(postedUpdate(currentFeed, rt::NotificationId{18}));
      REQUIRE(feedProjection.viewState().compact.kind == ActivityStatusKind::Info);

      feedProjection.handleFeedUpdated(expiredUpdate(feed({}), rt::NotificationId{18}));

      CHECK(feedProjection.viewState().compact.kind == ActivityStatusKind::Idle);
    }
  }

  TEST_CASE("ActivityStatusFeedProjection - local compact dismissal preserves notification histories",
            "[uimodel][unit][activity-status]")
  {
    auto feedProjection = ActivityStatusFeedProjection{ao::test::englishMessageCatalog(), feed({})};

    SECTION("compact dismiss does not remove detail feed")
    {
      auto currentFeed = feed({entry(
        rt::NotificationId{6}, rt::NotificationSeverity::Error, "Scan failed", rt::NotificationLifetime::pinned())});
      feedProjection.handleFeedUpdated(postedUpdate(currentFeed, rt::NotificationId{6}));
      REQUIRE(feedProjection.viewState().compact.kind == ActivityStatusKind::Error);
      CHECK(feedProjection.viewState().compact.text == "Scan failed");

      feedProjection.dismissCompact(currentFeed);

      CHECK(feedProjection.viewState().compact.kind == ActivityStatusKind::Idle);
      REQUIRE(feedProjection.viewState().detail.items.size() == 1);
      CHECK(feedProjection.viewState().detail.items[0].id == rt::NotificationId{6});
      CHECK(feedProjection.viewState().detail.items[0].message == "Scan failed");
    }

    SECTION("new persistent notification reappears after previous compact dismiss")
    {
      auto firstFeed = feed({entry(
        rt::NotificationId{7}, rt::NotificationSeverity::Error, "Old failure", rt::NotificationLifetime::pinned())});
      feedProjection.handleFeedUpdated(postedUpdate(firstFeed, rt::NotificationId{7}));
      REQUIRE(feedProjection.viewState().compact.kind == ActivityStatusKind::Error);
      CHECK(feedProjection.viewState().compact.text == "Old failure");
      feedProjection.dismissCompact(firstFeed);
      REQUIRE(feedProjection.viewState().compact.kind == ActivityStatusKind::Idle);

      auto nextFeed = feed(
        {entry(
           rt::NotificationId{7}, rt::NotificationSeverity::Error, "Old failure", rt::NotificationLifetime::pinned()),
         entry(
           rt::NotificationId{8}, rt::NotificationSeverity::Error, "New failure", rt::NotificationLifetime::pinned())});
      feedProjection.handleFeedUpdated(postedUpdate(nextFeed, rt::NotificationId{8}));

      CHECK(feedProjection.viewState().compact.kind == ActivityStatusKind::Error);
      CHECK(feedProjection.viewState().compact.text == "New failure");
      auto const& detail = feedProjection.viewState().detail;
      REQUIRE(detail.items.size() == 2);
      CHECK(detail.items[0].id == rt::NotificationId{8});
      CHECK(detail.items[0].message == "New failure");
      CHECK(detail.items[1].id == rt::NotificationId{7});
      CHECK(detail.items[1].message == "Old failure");
    }

    SECTION("dismissed higher severity does not suppress new lower severity persistent notification")
    {
      auto errorFeed = feed({entry(
        rt::NotificationId{16}, rt::NotificationSeverity::Error, "Old failure", rt::NotificationLifetime::pinned())});
      feedProjection.handleFeedUpdated(postedUpdate(errorFeed, rt::NotificationId{16}));
      REQUIRE(feedProjection.viewState().compact.kind == ActivityStatusKind::Error);
      feedProjection.dismissCompact(errorFeed);
      REQUIRE(feedProjection.viewState().compact.kind == ActivityStatusKind::Idle);

      auto warningFeed = feed(
        {entry(
           rt::NotificationId{16}, rt::NotificationSeverity::Error, "Old failure", rt::NotificationLifetime::pinned()),
         entry(rt::NotificationId{17},
               rt::NotificationSeverity::Warning,
               "New warning",
               rt::NotificationLifetime::pinned())});
      feedProjection.handleFeedUpdated(postedUpdate(warningFeed, rt::NotificationId{17}));

      CHECK(feedProjection.viewState().compact.kind == ActivityStatusKind::Warning);
      CHECK(feedProjection.viewState().compact.text == "New warning");
      auto const& detail = feedProjection.viewState().detail;
      REQUIRE(detail.items.size() == 2);
      CHECK(detail.items[0].id == rt::NotificationId{17});
      CHECK(detail.items[0].message == "New warning");
      CHECK(detail.items[1].id == rt::NotificationId{16});
      CHECK(detail.items[1].message == "Old failure");
    }

    SECTION("transient expiration returns to persistent warning when present")
    {
      auto currentFeed = feed({entry(rt::NotificationId{9}, rt::NotificationSeverity::Warning, "Partial import")});
      feedProjection.handleFeedUpdated(postedUpdate(currentFeed, rt::NotificationId{9}));
      feedProjection.dismissCompact(currentFeed);
      REQUIRE(feedProjection.viewState().compact.kind == ActivityStatusKind::Idle);

      auto temporaryFeed = feed({entry(rt::NotificationId{9}, rt::NotificationSeverity::Warning, "Partial import"),
                                 entry(rt::NotificationId{11}, rt::NotificationSeverity::Info, "Import saved")});
      feedProjection.handleFeedUpdated(postedUpdate(temporaryFeed, rt::NotificationId{11}));
      auto const& temporary = feedProjection.viewState().compact;
      REQUIRE(temporary.kind == ActivityStatusKind::Info);
      CHECK(temporary.text == "Import saved");
      REQUIRE(temporary.optAutoDismissTimeout);
      CHECK(*temporary.optAutoDismissTimeout == std::chrono::milliseconds{5000});

      auto replacementFeed = feed({entry(rt::NotificationId{10}, rt::NotificationSeverity::Warning, "New warning")});
      feedProjection.autoDismissCompact(replacementFeed);

      CHECK(feedProjection.viewState().compact.kind == ActivityStatusKind::Warning);
      CHECK(feedProjection.viewState().compact.text == "New warning");
    }
  }

  TEST_CASE("ActivityStatusFeedProjection - detail hiding updates the local compact projection",
            "[uimodel][unit][activity-status]")
  {
    auto currentFeed = feed({entry(rt::NotificationId{23}, rt::NotificationSeverity::Warning, "Older warning"),
                             entry(rt::NotificationId{24}, rt::NotificationSeverity::Warning, "Latest warning")});
    auto dismissProjection = ActivityStatusFeedProjection{ao::test::englishMessageCatalog(), currentFeed};
    REQUIRE(dismissProjection.viewState().detail.items.size() == 2);

    dismissProjection.hideDetailNotification(rt::NotificationId{24}, currentFeed);

    CHECK(currentFeed.entries.size() == 2);
    REQUIRE(dismissProjection.viewState().detail.items.size() == 1);
    CHECK(dismissProjection.viewState().detail.items[0].id == rt::NotificationId{23});
    CHECK(dismissProjection.viewState().compact.kind == ActivityStatusKind::Warning);
    CHECK(dismissProjection.viewState().compact.text == "Older warning");
  }
} // namespace ao::uimodel::test
