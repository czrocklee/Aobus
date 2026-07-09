// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/uimodel/status/activity/ActivityStatusFeedStateTestSupport.h"
#include "uimodel/status/activity/ActivityStatusFeedState.h"
#include <ao/rt/NotificationState.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#include <catch2/catch_test_macros.hpp>

#include <optional>

namespace ao::uimodel::test
{
  TEST_CASE("ActivityStatusFeedState - applies notification activity presentation policy",
            "[uimodel][unit][status][activity]")
  {
    auto feedState = ActivityStatusFeedState{};

    SECTION("hidden notification presentation is ignored by activity status")
    {
      auto currentFeed = feed({entry(rt::NotificationId{11},
                                     rt::NotificationSeverity::Info,
                                     "Aobus Ready",
                                     false,
                                     std::nullopt,
                                     rt::NotificationActivityPresentation::Hidden)});

      feedState.handleNotificationPosted(currentFeed, rt::NotificationId{11});

      CHECK(feedState.viewState().compact.kind == ActivityStatusKind::Idle);
      CHECK(feedState.viewState().detail.items.empty());
    }

    SECTION("detail-only notification presentation does not create compact state")
    {
      auto currentFeed = feed({entry(rt::NotificationId{19},
                                     rt::NotificationSeverity::Info,
                                     "Index diagnostic",
                                     false,
                                     std::nullopt,
                                     rt::NotificationActivityPresentation::DetailOnly)});

      feedState.handleNotificationPosted(currentFeed, rt::NotificationId{19});

      CHECK(feedState.viewState().compact.kind == ActivityStatusKind::Idle);
      REQUIRE(feedState.viewState().detail.items.size() == 1);
      CHECK(feedState.viewState().detail.items[0].message == "Index diagnostic");
      CHECK(hasDetailContent(feedState.viewState().detail));
    }

    SECTION("non-compact presentations do not interrupt library progress")
    {
      feedState.handleLibraryTaskProgress("Scanning: album.flac", 0.4);

      auto currentFeed = feed({entry(rt::NotificationId{25},
                                     rt::NotificationSeverity::Info,
                                     "Index diagnostic",
                                     false,
                                     std::nullopt,
                                     rt::NotificationActivityPresentation::DetailOnly)});
      feedState.handleNotificationPosted(currentFeed, rt::NotificationId{25});

      CHECK(feedState.viewState().compact.kind == ActivityStatusKind::Processing);
      CHECK(feedState.viewState().compact.text == "Scanning library");
      REQUIRE(feedState.viewState().detail.items.size() == 1);
      CHECK(feedState.viewState().detail.items[0].message == "Index diagnostic");

      feedState.handleLibraryTaskCompleted(3, currentFeed);

      CHECK(feedState.viewState().compact.kind == ActivityStatusKind::Success);
      CHECK(feedState.viewState().compact.text == "Scan complete: 3 tracks added");
    }

    SECTION("hidden presentation does not interrupt library progress")
    {
      feedState.handleLibraryTaskProgress("Updating: album.flac", 0.7);

      auto currentFeed = feed({entry(rt::NotificationId{26},
                                     rt::NotificationSeverity::Info,
                                     "Aobus Ready",
                                     false,
                                     std::nullopt,
                                     rt::NotificationActivityPresentation::Hidden)});
      feedState.handleNotificationPosted(currentFeed, rt::NotificationId{26});

      CHECK(feedState.viewState().compact.kind == ActivityStatusKind::Processing);
      CHECK(feedState.viewState().compact.text == "Updating library");
      CHECK(feedState.viewState().detail.items.empty());
    }

    SECTION("non-compact presentations do not replace transient compact state")
    {
      auto info = entry(rt::NotificationId{27}, rt::NotificationSeverity::Info, "Saved playlist");
      feedState.handleNotificationPosted(feed({info}), rt::NotificationId{27});
      REQUIRE(feedState.viewState().compact.kind == ActivityStatusKind::Info);

      auto detailOnly = entry(rt::NotificationId{28},
                              rt::NotificationSeverity::Info,
                              "Index diagnostic",
                              false,
                              std::nullopt,
                              rt::NotificationActivityPresentation::DetailOnly);
      feedState.handleNotificationPosted(feed({info, detailOnly}), rt::NotificationId{28});

      CHECK(feedState.viewState().compact.kind == ActivityStatusKind::Info);
      CHECK(feedState.viewState().compact.text == "Saved playlist");
      REQUIRE(feedState.viewState().detail.items.size() == 1);
      CHECK(feedState.viewState().detail.items[0].id == rt::NotificationId{28});
    }

    SECTION("default notification presentation remains compact eligible")
    {
      auto currentFeed = feed({entry(rt::NotificationId{20}, rt::NotificationSeverity::Warning, "Default warning")});

      feedState.handleNotificationPosted(currentFeed, rt::NotificationId{20});

      CHECK(feedState.viewState().compact.kind == ActivityStatusKind::Warning);
      CHECK(feedState.viewState().compact.text == "Default warning");
      REQUIRE(feedState.viewState().detail.items.size() == 1);
    }

    SECTION("plain default info creates compact state without openable detail")
    {
      auto currentFeed = feed({entry(rt::NotificationId{21}, rt::NotificationSeverity::Info, "Saved playlist")});

      feedState.handleNotificationPosted(currentFeed, rt::NotificationId{21});

      CHECK(feedState.viewState().compact.kind == ActivityStatusKind::Info);
      CHECK(feedState.viewState().compact.text == "Saved playlist");
      CHECK_FALSE(feedState.viewState().compact.hasDetails);
      CHECK(feedState.viewState().detail.items.empty());
      CHECK_FALSE(hasDetailContent(feedState.viewState().detail));
    }

    SECTION("sticky info remains transient compact while staying detail eligible")
    {
      auto currentFeed = feed({entry(rt::NotificationId{29}, rt::NotificationSeverity::Info, "Background note", true)});

      feedState.handleNotificationPosted(currentFeed, rt::NotificationId{29});

      CHECK(feedState.viewState().compact.kind == ActivityStatusKind::Info);
      CHECK(feedState.viewState().compact.text == "Background note");
      CHECK_FALSE(feedState.viewState().compact.persistent);
      CHECK(feedState.viewState().compact.optAutoDismissTimeout == kActivityStatusDefaultAutoDismissTimeout);
      REQUIRE(feedState.viewState().detail.items.size() == 1);
      CHECK(feedState.viewState().detail.items[0].sticky);
    }
  }
} // namespace ao::uimodel::test
