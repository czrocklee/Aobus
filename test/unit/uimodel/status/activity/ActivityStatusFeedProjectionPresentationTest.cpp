// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/uimodel/status/activity/ActivityStatusFeedProjectionTestSupport.h"
#include "uimodel/status/activity/ActivityStatusFeedProjection.h"
#include <ao/rt/NotificationState.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#include <catch2/catch_test_macros.hpp>

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
                                     rt::NotificationLifetime::transient(),
                                     rt::NotificationActivityPresentation::Hidden)});

      feedProjection.handleFeedUpdated(postedUpdate(currentFeed, rt::NotificationId{11}));

      CHECK(feedProjection.viewState().compact.kind == ActivityStatusKind::Idle);
      CHECK(feedProjection.viewState().detail.items.empty());
    }

    SECTION("detail-only notification presentation does not create compact state")
    {
      auto currentFeed = feed({entry(rt::NotificationId{19},
                                     rt::NotificationSeverity::Info,
                                     "Index diagnostic",
                                     rt::NotificationLifetime::sessionHistory(),
                                     rt::NotificationActivityPresentation::DetailOnly)});

      feedProjection.handleFeedUpdated(postedUpdate(currentFeed, rt::NotificationId{19}));

      CHECK(feedProjection.viewState().compact.kind == ActivityStatusKind::Idle);
      REQUIRE(feedProjection.viewState().detail.items.size() == 1);
      CHECK(feedProjection.viewState().detail.items[0].message == "Index diagnostic");
      CHECK(hasDetailContent(feedProjection.viewState().detail));
    }

    SECTION("non-compact presentations do not interrupt library progress")
    {
      feedProjection.handleLibraryTaskProgress(
        libraryTaskProgress(rt::LibraryChanges::LibraryTaskProgressKind::Scanning, "album.flac", 0.4));

      auto currentFeed = feed({entry(rt::NotificationId{25},
                                     rt::NotificationSeverity::Info,
                                     "Index diagnostic",
                                     rt::NotificationLifetime::sessionHistory(),
                                     rt::NotificationActivityPresentation::DetailOnly)});
      feedProjection.handleFeedUpdated(postedUpdate(currentFeed, rt::NotificationId{25}));

      CHECK(feedProjection.viewState().compact.kind == ActivityStatusKind::Processing);
      CHECK(feedProjection.viewState().compact.text == "Scanning library");
      REQUIRE(feedProjection.viewState().detail.items.size() == 1);
      CHECK(feedProjection.viewState().detail.items[0].message == "Index diagnostic");

      feedProjection.handleLibraryTaskCompleted(libraryTaskCompletion(3), currentFeed);

      CHECK(feedProjection.viewState().compact.kind == ActivityStatusKind::Success);
      CHECK(feedProjection.viewState().compact.text == "Scan complete: 3 tracks added");
    }

    SECTION("hidden presentation does not interrupt library progress")
    {
      feedProjection.handleLibraryTaskProgress(
        libraryTaskProgress(rt::LibraryChanges::LibraryTaskProgressKind::Updating, "album.flac", 0.7));

      auto currentFeed = feed({entry(rt::NotificationId{26},
                                     rt::NotificationSeverity::Info,
                                     "Aobus Ready",
                                     rt::NotificationLifetime::transient(),
                                     rt::NotificationActivityPresentation::Hidden)});
      feedProjection.handleFeedUpdated(postedUpdate(currentFeed, rt::NotificationId{26}));

      CHECK(feedProjection.viewState().compact.kind == ActivityStatusKind::Processing);
      CHECK(feedProjection.viewState().compact.text == "Updating library");
      CHECK(feedProjection.viewState().detail.items.empty());
    }

    SECTION("non-compact presentations do not replace transient compact state")
    {
      auto info = entry(rt::NotificationId{27}, rt::NotificationSeverity::Info, "Saved playlist");
      feedProjection.handleFeedUpdated(postedUpdate(feed({info}), rt::NotificationId{27}));
      REQUIRE(feedProjection.viewState().compact.kind == ActivityStatusKind::Info);

      auto detailOnly = entry(rt::NotificationId{28},
                              rt::NotificationSeverity::Info,
                              "Index diagnostic",
                              rt::NotificationLifetime::sessionHistory(),
                              rt::NotificationActivityPresentation::DetailOnly);
      feedProjection.handleFeedUpdated(postedUpdate(feed({info, detailOnly}), rt::NotificationId{28}));

      CHECK(feedProjection.viewState().compact.kind == ActivityStatusKind::Info);
      CHECK(feedProjection.viewState().compact.text == "Saved playlist");
      REQUIRE(feedProjection.viewState().detail.items.size() == 1);
      CHECK(feedProjection.viewState().detail.items[0].id == rt::NotificationId{28});
    }

    SECTION("default notification presentation remains compact eligible")
    {
      auto currentFeed = feed({entry(rt::NotificationId{20}, rt::NotificationSeverity::Warning, "Default warning")});

      feedProjection.handleFeedUpdated(postedUpdate(currentFeed, rt::NotificationId{20}));

      CHECK(feedProjection.viewState().compact.kind == ActivityStatusKind::Warning);
      CHECK(feedProjection.viewState().compact.text == "Default warning");
      REQUIRE(feedProjection.viewState().detail.items.size() == 1);
    }

    SECTION("plain default info creates compact state without openable detail")
    {
      auto currentFeed = feed({entry(rt::NotificationId{21}, rt::NotificationSeverity::Info, "Saved playlist")});

      feedProjection.handleFeedUpdated(postedUpdate(currentFeed, rt::NotificationId{21}));

      CHECK(feedProjection.viewState().compact.kind == ActivityStatusKind::Info);
      CHECK(feedProjection.viewState().compact.text == "Saved playlist");
      CHECK_FALSE(feedProjection.viewState().compact.hasDetails);
      CHECK(feedProjection.viewState().detail.items.empty());
      CHECK_FALSE(hasDetailContent(feedProjection.viewState().detail));
    }

    SECTION("until-dismissed info stays detail eligible while compact remains presentation-transient")
    {
      auto currentFeed = feed({entry(rt::NotificationId{29},
                                     rt::NotificationSeverity::Info,
                                     "Background note",
                                     rt::NotificationLifetime::untilDismissed())});

      feedProjection.handleFeedUpdated(postedUpdate(currentFeed, rt::NotificationId{29}));

      CHECK(feedProjection.viewState().compact.kind == ActivityStatusKind::Info);
      CHECK(feedProjection.viewState().compact.text == "Background note");
      CHECK_FALSE(feedProjection.viewState().compact.persistent);
      CHECK(feedProjection.viewState().compact.optAutoDismissTimeout == kActivityStatusDefaultAutoDismissTimeout);
      REQUIRE(feedProjection.viewState().detail.items.size() == 1);
      CHECK_FALSE(feedProjection.viewState().detail.items[0].dismissible);
    }
  }
} // namespace ao::uimodel::test
