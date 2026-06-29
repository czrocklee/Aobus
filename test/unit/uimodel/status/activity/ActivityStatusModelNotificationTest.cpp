// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/uimodel/status/activity/ActivityStatusModelTestSupport.h"
#include <ao/rt/NotificationState.h>
#include <ao/uimodel/status/activity/ActivityStatusModel.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>

namespace ao::uimodel::test
{
  TEST_CASE("ActivityStatusModel projects compact notifications and dismissal state",
            "[uimodel][unit][status][activity]")
  {
    auto model = ActivityStatusModel{};

    SECTION("warning and error notifications are severity-grouped")
    {
      auto currentFeed = feed({entry(rt::NotificationId{2}, rt::NotificationSeverity::Warning, "Warn A"),
                               entry(rt::NotificationId{3}, rt::NotificationSeverity::Error, "Error A"),
                               entry(rt::NotificationId{4}, rt::NotificationSeverity::Error, "Error B")});

      model.initialize(currentFeed);

      auto const& compact = model.viewState().compact;
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

      model.onNotificationPosted(currentFeed, rt::NotificationId{5});

      auto const& compact = model.viewState().compact;
      CHECK(compact.kind == ActivityStatusKind::Info);
      CHECK(compact.text == "Saved playlist");
      CHECK(compact.optAutoDismissTimeout == std::chrono::milliseconds{1500});
    }

    SECTION("compact dismiss does not remove detail feed")
    {
      auto currentFeed = feed({entry(rt::NotificationId{6}, rt::NotificationSeverity::Error, "Scan failed", true)});
      model.onNotificationPosted(currentFeed, rt::NotificationId{6});

      model.dismissCompact(currentFeed);

      CHECK(model.viewState().compact.kind == ActivityStatusKind::Idle);
      REQUIRE(model.viewState().detail.items.size() == 1);
      CHECK(model.viewState().detail.items[0].message == "Scan failed");
    }

    SECTION("new persistent notification reappears after previous compact dismiss")
    {
      auto firstFeed = feed({entry(rt::NotificationId{7}, rt::NotificationSeverity::Error, "Old failure", true)});
      model.onNotificationPosted(firstFeed, rt::NotificationId{7});
      model.dismissCompact(firstFeed);

      auto nextFeed = feed({entry(rt::NotificationId{7}, rt::NotificationSeverity::Error, "Old failure", true),
                            entry(rt::NotificationId{8}, rt::NotificationSeverity::Error, "New failure", true)});
      model.onNotificationPosted(nextFeed, rt::NotificationId{8});

      CHECK(model.viewState().compact.kind == ActivityStatusKind::Error);
      CHECK(model.viewState().compact.text == "New failure");
      CHECK(model.viewState().compact.groupedCount == 1);
      REQUIRE(model.viewState().detail.items.size() == 2);
    }

    SECTION("dismissed higher severity does not suppress new lower severity persistent notification")
    {
      auto errorFeed = feed({entry(rt::NotificationId{16}, rt::NotificationSeverity::Error, "Old failure", true)});
      model.onNotificationPosted(errorFeed, rt::NotificationId{16});
      model.dismissCompact(errorFeed);

      auto warningFeed = feed({entry(rt::NotificationId{16}, rt::NotificationSeverity::Error, "Old failure", true),
                               entry(rt::NotificationId{17}, rt::NotificationSeverity::Warning, "New warning", true)});
      model.onNotificationPosted(warningFeed, rt::NotificationId{17});

      CHECK(model.viewState().compact.kind == ActivityStatusKind::Warning);
      CHECK(model.viewState().compact.text == "New warning");
      CHECK(model.viewState().compact.groupedCount == 1);
    }

    SECTION("notification-derived transient disappears when its source leaves the feed")
    {
      auto currentFeed = feed({entry(rt::NotificationId{18}, rt::NotificationSeverity::Info, "Saved playlist")});
      model.onNotificationPosted(currentFeed, rt::NotificationId{18});
      REQUIRE(model.viewState().compact.kind == ActivityStatusKind::Info);

      model.onFeedChanged(feed({}));

      CHECK(model.viewState().compact.kind == ActivityStatusKind::Idle);
    }

    SECTION("transient expiration returns to persistent warning when present")
    {
      auto currentFeed = feed({entry(rt::NotificationId{9}, rt::NotificationSeverity::Warning, "Partial import")});
      model.onNotificationPosted(currentFeed, rt::NotificationId{9});
      model.dismissCompact(currentFeed);

      auto replacementFeed = feed({entry(rt::NotificationId{10}, rt::NotificationSeverity::Warning, "New warning")});
      model.onTransientExpired(replacementFeed);

      CHECK(model.viewState().compact.kind == ActivityStatusKind::Warning);
      CHECK(model.viewState().compact.text == "New warning");
    }

    SECTION("detail dismiss locally hides a clearable notification and updates compact projection")
    {
      auto currentFeed = feed({entry(rt::NotificationId{23}, rt::NotificationSeverity::Warning, "Older warning"),
                               entry(rt::NotificationId{24}, rt::NotificationSeverity::Warning, "Latest warning")});

      model.initialize(currentFeed);
      REQUIRE(model.viewState().detail.items.size() == 2);
      REQUIRE(model.viewState().compact.sourceNotificationIds.size() == 2);

      model.hideDetailNotificationFromActivity(rt::NotificationId{24}, currentFeed);

      CHECK(currentFeed.entries.size() == 2);
      REQUIRE(model.viewState().detail.items.size() == 1);
      CHECK(model.viewState().detail.items[0].id == rt::NotificationId{23});
      CHECK(model.viewState().compact.kind == ActivityStatusKind::Warning);
      CHECK(model.viewState().compact.text == "Older warning");
      REQUIRE(model.viewState().compact.sourceNotificationIds.size() == 1);
      CHECK(model.viewState().compact.sourceNotificationIds[0] == rt::NotificationId{23});

      auto const hideableIds = model.locallyHideableNotificationIds(currentFeed);
      REQUIRE(hideableIds.size() == 1);
      CHECK(hideableIds[0] == rt::NotificationId{23});
    }
  }
} // namespace ao::uimodel::test
