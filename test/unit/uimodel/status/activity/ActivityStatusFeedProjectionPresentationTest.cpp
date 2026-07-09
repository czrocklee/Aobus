// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/uimodel/status/activity/ActivityStatusFeedProjectionTestSupport.h"
#include "uimodel/status/activity/ActivityStatusFeedProjection.h"
#include <ao/rt/NotificationState.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#include <catch2/catch_test_macros.hpp>

#include <optional>

namespace ao::uimodel::test
{
  TEST_CASE("ActivityStatusFeedProjection - applies notification activity presentation policy",
            "[uimodel][unit][status][activity]")
  {
    auto feedProjection = ActivityStatusFeedProjection{};

    SECTION("hidden notification presentation is ignored by activity status")
    {
      auto currentFeed = feed({entry(rt::NotificationId{11},
                                     rt::NotificationSeverity::Info,
                                     "Aobus Ready",
                                     false,
                                     std::nullopt,
                                     rt::NotificationActivityPresentation::Hidden)});

      feedProjection.handleNotificationPosted(currentFeed, rt::NotificationId{11});

      CHECK(feedProjection.viewState().compact.kind == ActivityStatusKind::Idle);
      CHECK(feedProjection.viewState().detail.items.empty());
    }

    SECTION("detail-only notification presentation does not create compact state")
    {
      auto currentFeed = feed({entry(rt::NotificationId{19},
                                     rt::NotificationSeverity::Info,
                                     "Index diagnostic",
                                     false,
                                     std::nullopt,
                                     rt::NotificationActivityPresentation::DetailOnly)});

      feedProjection.handleNotificationPosted(currentFeed, rt::NotificationId{19});

      CHECK(feedProjection.viewState().compact.kind == ActivityStatusKind::Idle);
      REQUIRE(feedProjection.viewState().detail.items.size() == 1);
      CHECK(feedProjection.viewState().detail.items[0].message == "Index diagnostic");
      CHECK(hasDetailContent(feedProjection.viewState().detail));
    }

    SECTION("non-compact presentations do not interrupt library progress")
    {
      feedProjection.handleLibraryTaskProgress("Scanning: album.flac", 0.4);

      auto currentFeed = feed({entry(rt::NotificationId{25},
                                     rt::NotificationSeverity::Info,
                                     "Index diagnostic",
                                     false,
                                     std::nullopt,
                                     rt::NotificationActivityPresentation::DetailOnly)});
      feedProjection.handleNotificationPosted(currentFeed, rt::NotificationId{25});

      CHECK(feedProjection.viewState().compact.kind == ActivityStatusKind::Processing);
      CHECK(feedProjection.viewState().compact.text == "Scanning library");
      REQUIRE(feedProjection.viewState().detail.items.size() == 1);
      CHECK(feedProjection.viewState().detail.items[0].message == "Index diagnostic");

      feedProjection.handleLibraryTaskCompleted(3, currentFeed);

      CHECK(feedProjection.viewState().compact.kind == ActivityStatusKind::Success);
      CHECK(feedProjection.viewState().compact.text == "Scan complete: 3 tracks added");
    }

    SECTION("hidden presentation does not interrupt library progress")
    {
      feedProjection.handleLibraryTaskProgress("Updating: album.flac", 0.7);

      auto currentFeed = feed({entry(rt::NotificationId{26},
                                     rt::NotificationSeverity::Info,
                                     "Aobus Ready",
                                     false,
                                     std::nullopt,
                                     rt::NotificationActivityPresentation::Hidden)});
      feedProjection.handleNotificationPosted(currentFeed, rt::NotificationId{26});

      CHECK(feedProjection.viewState().compact.kind == ActivityStatusKind::Processing);
      CHECK(feedProjection.viewState().compact.text == "Updating library");
      CHECK(feedProjection.viewState().detail.items.empty());
    }

    SECTION("non-compact presentations do not replace transient compact state")
    {
      auto info = entry(rt::NotificationId{27}, rt::NotificationSeverity::Info, "Saved playlist");
      feedProjection.handleNotificationPosted(feed({info}), rt::NotificationId{27});
      REQUIRE(feedProjection.viewState().compact.kind == ActivityStatusKind::Info);

      auto detailOnly = entry(rt::NotificationId{28},
                              rt::NotificationSeverity::Info,
                              "Index diagnostic",
                              false,
                              std::nullopt,
                              rt::NotificationActivityPresentation::DetailOnly);
      feedProjection.handleNotificationPosted(feed({info, detailOnly}), rt::NotificationId{28});

      CHECK(feedProjection.viewState().compact.kind == ActivityStatusKind::Info);
      CHECK(feedProjection.viewState().compact.text == "Saved playlist");
      REQUIRE(feedProjection.viewState().detail.items.size() == 1);
      CHECK(feedProjection.viewState().detail.items[0].id == rt::NotificationId{28});
    }

    SECTION("default notification presentation remains compact eligible")
    {
      auto currentFeed = feed({entry(rt::NotificationId{20}, rt::NotificationSeverity::Warning, "Default warning")});

      feedProjection.handleNotificationPosted(currentFeed, rt::NotificationId{20});

      CHECK(feedProjection.viewState().compact.kind == ActivityStatusKind::Warning);
      CHECK(feedProjection.viewState().compact.text == "Default warning");
      REQUIRE(feedProjection.viewState().detail.items.size() == 1);
    }

    SECTION("plain default info creates compact state without openable detail")
    {
      auto currentFeed = feed({entry(rt::NotificationId{21}, rt::NotificationSeverity::Info, "Saved playlist")});

      feedProjection.handleNotificationPosted(currentFeed, rt::NotificationId{21});

      CHECK(feedProjection.viewState().compact.kind == ActivityStatusKind::Info);
      CHECK(feedProjection.viewState().compact.text == "Saved playlist");
      CHECK_FALSE(feedProjection.viewState().compact.hasDetails);
      CHECK(feedProjection.viewState().detail.items.empty());
      CHECK_FALSE(hasDetailContent(feedProjection.viewState().detail));
    }

    SECTION("sticky info remains transient compact while staying detail eligible")
    {
      auto currentFeed = feed({entry(rt::NotificationId{29}, rt::NotificationSeverity::Info, "Background note", true)});

      feedProjection.handleNotificationPosted(currentFeed, rt::NotificationId{29});

      CHECK(feedProjection.viewState().compact.kind == ActivityStatusKind::Info);
      CHECK(feedProjection.viewState().compact.text == "Background note");
      CHECK_FALSE(feedProjection.viewState().compact.persistent);
      CHECK(feedProjection.viewState().compact.optAutoDismissTimeout == kActivityStatusDefaultAutoDismissTimeout);
      REQUIRE(feedProjection.viewState().detail.items.size() == 1);
      CHECK(feedProjection.viewState().detail.items[0].sticky);
    }
  }
} // namespace ao::uimodel::test
