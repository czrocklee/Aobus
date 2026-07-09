// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/uimodel/status/activity/ActivityStatusFeedStateTestSupport.h"
#include "uimodel/status/activity/ActivityStatusFeedState.h"
#include <ao/rt/NotificationState.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>

namespace ao::uimodel::test
{
  TEST_CASE("ActivityStatusFeedState - projects compact notifications and dismissal state",
            "[uimodel][unit][status][activity]")
  {
    auto feedState = ActivityStatusFeedState{};

    SECTION("warning and error notifications are severity-grouped")
    {
      auto currentFeed = feed({entry(rt::NotificationId{2}, rt::NotificationSeverity::Warning, "Warn A"),
                               entry(rt::NotificationId{3}, rt::NotificationSeverity::Error, "Error A"),
                               entry(rt::NotificationId{4}, rt::NotificationSeverity::Error, "Error B")});

      feedState.initialize(currentFeed);

      auto const& compact = feedState.viewState().compact;
      CHECK(compact.kind == ActivityStatusKind::Error);
      CHECK(compact.text == "2 errors");
      CHECK(compact.groupedCount == 2);
      CHECK(compact.hasDetails);
      REQUIRE(compact.sourceNotificationIds.size() == 2);
    }

    SECTION("info notifications use custom transient timeout")
    {
      auto currentFeed = feed({entry(rt::NotificationId{5},
                                     rt::NotificationSeverity::Info,
                                     "Saved playlist",
                                     false,
                                     std::chrono::milliseconds{1500})});

      feedState.handleNotificationPosted(currentFeed, rt::NotificationId{5});

      auto const& compact = feedState.viewState().compact;
      CHECK(compact.kind == ActivityStatusKind::Info);
      CHECK(compact.text == "Saved playlist");
      CHECK(compact.optAutoDismissTimeout == std::chrono::milliseconds{1500});
    }

    SECTION("compact dismiss does not remove detail feed")
    {
      auto currentFeed = feed({entry(rt::NotificationId{6}, rt::NotificationSeverity::Error, "Scan failed", true)});
      feedState.handleNotificationPosted(currentFeed, rt::NotificationId{6});

      feedState.dismissCompact(currentFeed);

      CHECK(feedState.viewState().compact.kind == ActivityStatusKind::Idle);
      REQUIRE(feedState.viewState().detail.items.size() == 1);
      CHECK(feedState.viewState().detail.items[0].message == "Scan failed");
    }

    SECTION("new persistent notification reappears after previous compact dismiss")
    {
      auto firstFeed = feed({entry(rt::NotificationId{7}, rt::NotificationSeverity::Error, "Old failure", true)});
      feedState.handleNotificationPosted(firstFeed, rt::NotificationId{7});
      feedState.dismissCompact(firstFeed);

      auto nextFeed = feed({entry(rt::NotificationId{7}, rt::NotificationSeverity::Error, "Old failure", true),
                            entry(rt::NotificationId{8}, rt::NotificationSeverity::Error, "New failure", true)});
      feedState.handleNotificationPosted(nextFeed, rt::NotificationId{8});

      CHECK(feedState.viewState().compact.kind == ActivityStatusKind::Error);
      CHECK(feedState.viewState().compact.text == "New failure");
      CHECK(feedState.viewState().compact.groupedCount == 1);
      REQUIRE(feedState.viewState().detail.items.size() == 2);
    }

    SECTION("dismissed higher severity does not suppress new lower severity persistent notification")
    {
      auto errorFeed = feed({entry(rt::NotificationId{16}, rt::NotificationSeverity::Error, "Old failure", true)});
      feedState.handleNotificationPosted(errorFeed, rt::NotificationId{16});
      feedState.dismissCompact(errorFeed);

      auto warningFeed = feed({entry(rt::NotificationId{16}, rt::NotificationSeverity::Error, "Old failure", true),
                               entry(rt::NotificationId{17}, rt::NotificationSeverity::Warning, "New warning", true)});
      feedState.handleNotificationPosted(warningFeed, rt::NotificationId{17});

      CHECK(feedState.viewState().compact.kind == ActivityStatusKind::Warning);
      CHECK(feedState.viewState().compact.text == "New warning");
      CHECK(feedState.viewState().compact.groupedCount == 1);
    }

    SECTION("notification-derived transient disappears when its source leaves the feed")
    {
      auto currentFeed = feed({entry(rt::NotificationId{18}, rt::NotificationSeverity::Info, "Saved playlist")});
      feedState.handleNotificationPosted(currentFeed, rt::NotificationId{18});
      REQUIRE(feedState.viewState().compact.kind == ActivityStatusKind::Info);

      feedState.handleFeedChanged(feed({}));

      CHECK(feedState.viewState().compact.kind == ActivityStatusKind::Idle);
    }

    SECTION("transient expiration returns to persistent warning when present")
    {
      auto currentFeed = feed({entry(rt::NotificationId{9}, rt::NotificationSeverity::Warning, "Partial import")});
      feedState.handleNotificationPosted(currentFeed, rt::NotificationId{9});
      feedState.dismissCompact(currentFeed);

      auto replacementFeed = feed({entry(rt::NotificationId{10}, rt::NotificationSeverity::Warning, "New warning")});
      feedState.handleTransientExpired(replacementFeed);

      CHECK(feedState.viewState().compact.kind == ActivityStatusKind::Warning);
      CHECK(feedState.viewState().compact.text == "New warning");
    }

    SECTION("detail dismiss locally hides a clearable notification and updates compact projection")
    {
      auto currentFeed = feed({entry(rt::NotificationId{23}, rt::NotificationSeverity::Warning, "Older warning"),
                               entry(rt::NotificationId{24}, rt::NotificationSeverity::Warning, "Latest warning")});

      feedState.initialize(currentFeed);
      REQUIRE(feedState.viewState().detail.items.size() == 2);
      REQUIRE(feedState.viewState().compact.sourceNotificationIds.size() == 2);

      feedState.dismissDetailNotificationFromActivity(rt::NotificationId{24}, currentFeed);

      CHECK(currentFeed.entries.size() == 2);
      REQUIRE(feedState.viewState().detail.items.size() == 1);
      CHECK(feedState.viewState().detail.items[0].id == rt::NotificationId{23});
      CHECK(feedState.viewState().compact.kind == ActivityStatusKind::Warning);
      CHECK(feedState.viewState().compact.text == "Older warning");
      REQUIRE(feedState.viewState().compact.sourceNotificationIds.size() == 1);
      CHECK(feedState.viewState().compact.sourceNotificationIds[0] == rt::NotificationId{23});

      auto const hideableIds = feedState.locallyHideableNotificationIds(currentFeed);
      REQUIRE(hideableIds.size() == 1);
      CHECK(hideableIds[0] == rt::NotificationId{23});
    }
  }
} // namespace ao::uimodel::test
