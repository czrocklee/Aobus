// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/uimodel/status/activity/ActivityStatusFeedProjectionTestSupport.h"
#include "uimodel/status/activity/ActivityStatusFeedProjection.h"
#include <ao/rt/NotificationState.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>

namespace ao::uimodel::test
{
  TEST_CASE("ActivityStatusFeedProjection - projects compact notifications and dismissal state",
            "[uimodel][unit][status][activity]")
  {
    auto feedProjection = ActivityStatusFeedProjection{};

    SECTION("warning and error notifications are severity-grouped")
    {
      auto currentFeed = feed({entry(rt::NotificationId{2}, rt::NotificationSeverity::Warning, "Warn A"),
                               entry(rt::NotificationId{3}, rt::NotificationSeverity::Error, "Error A"),
                               entry(rt::NotificationId{4}, rt::NotificationSeverity::Error, "Error B")});

      feedProjection.initialize(currentFeed);

      auto const& compact = feedProjection.viewState().compact;
      CHECK(compact.kind == ActivityStatusKind::Error);
      CHECK(compact.text == "2 errors");
      CHECK(compact.hasDetails);
    }

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

    SECTION("compact dismiss does not remove detail feed")
    {
      auto currentFeed = feed({entry(
        rt::NotificationId{6}, rt::NotificationSeverity::Error, "Scan failed", rt::NotificationLifetime::pinned())});
      feedProjection.handleFeedUpdated(postedUpdate(currentFeed, rt::NotificationId{6}));

      feedProjection.dismissCompact(currentFeed);

      CHECK(feedProjection.viewState().compact.kind == ActivityStatusKind::Idle);
      REQUIRE(feedProjection.viewState().detail.items.size() == 1);
      CHECK(feedProjection.viewState().detail.items[0].message == "Scan failed");
    }

    SECTION("new persistent notification reappears after previous compact dismiss")
    {
      auto firstFeed = feed({entry(
        rt::NotificationId{7}, rt::NotificationSeverity::Error, "Old failure", rt::NotificationLifetime::pinned())});
      feedProjection.handleFeedUpdated(postedUpdate(firstFeed, rt::NotificationId{7}));
      feedProjection.dismissCompact(firstFeed);

      auto nextFeed = feed(
        {entry(
           rt::NotificationId{7}, rt::NotificationSeverity::Error, "Old failure", rt::NotificationLifetime::pinned()),
         entry(
           rt::NotificationId{8}, rt::NotificationSeverity::Error, "New failure", rt::NotificationLifetime::pinned())});
      feedProjection.handleFeedUpdated(postedUpdate(nextFeed, rt::NotificationId{8}));

      CHECK(feedProjection.viewState().compact.kind == ActivityStatusKind::Error);
      CHECK(feedProjection.viewState().compact.text == "New failure");
      REQUIRE(feedProjection.viewState().detail.items.size() == 2);
    }

    SECTION("dismissed higher severity does not suppress new lower severity persistent notification")
    {
      auto errorFeed = feed({entry(
        rt::NotificationId{16}, rt::NotificationSeverity::Error, "Old failure", rt::NotificationLifetime::pinned())});
      feedProjection.handleFeedUpdated(postedUpdate(errorFeed, rt::NotificationId{16}));
      feedProjection.dismissCompact(errorFeed);

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
    }

    SECTION("notification-derived transient disappears when its source leaves the feed")
    {
      auto currentFeed = feed({entry(rt::NotificationId{18}, rt::NotificationSeverity::Info, "Saved playlist")});
      feedProjection.handleFeedUpdated(postedUpdate(currentFeed, rt::NotificationId{18}));
      REQUIRE(feedProjection.viewState().compact.kind == ActivityStatusKind::Info);

      feedProjection.handleFeedUpdated(expiredUpdate(feed({}), rt::NotificationId{18}));

      CHECK(feedProjection.viewState().compact.kind == ActivityStatusKind::Idle);
    }

    SECTION("transient expiration returns to persistent warning when present")
    {
      auto currentFeed = feed({entry(rt::NotificationId{9}, rt::NotificationSeverity::Warning, "Partial import")});
      feedProjection.handleFeedUpdated(postedUpdate(currentFeed, rt::NotificationId{9}));
      feedProjection.dismissCompact(currentFeed);

      auto replacementFeed = feed({entry(rt::NotificationId{10}, rt::NotificationSeverity::Warning, "New warning")});
      feedProjection.autoDismissCompact(replacementFeed);

      CHECK(feedProjection.viewState().compact.kind == ActivityStatusKind::Warning);
      CHECK(feedProjection.viewState().compact.text == "New warning");
    }

    SECTION("detail dismiss locally hides a clearable notification and updates compact projection")
    {
      auto currentFeed = feed({entry(rt::NotificationId{23}, rt::NotificationSeverity::Warning, "Older warning"),
                               entry(rt::NotificationId{24}, rt::NotificationSeverity::Warning, "Latest warning")});

      feedProjection.initialize(currentFeed);
      REQUIRE(feedProjection.viewState().detail.items.size() == 2);

      feedProjection.hideDetailNotification(rt::NotificationId{24}, currentFeed);

      CHECK(currentFeed.entries.size() == 2);
      REQUIRE(feedProjection.viewState().detail.items.size() == 1);
      CHECK(feedProjection.viewState().detail.items[0].id == rt::NotificationId{23});
      CHECK(feedProjection.viewState().compact.kind == ActivityStatusKind::Warning);
      CHECK(feedProjection.viewState().compact.text == "Older warning");
    }
  }
} // namespace ao::uimodel::test
