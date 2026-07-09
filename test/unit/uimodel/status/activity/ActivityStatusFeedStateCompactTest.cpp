// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/uimodel/status/activity/ActivityStatusFeedStateTestSupport.h"
#include "uimodel/status/activity/ActivityStatusFeedState.h"
#include <ao/rt/NotificationState.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::uimodel::test
{
  TEST_CASE("ActivityStatusFeedState - projects compact state from runtime priority",
            "[uimodel][unit][status][activity]")
  {
    auto feedState = ActivityStatusFeedState{};

    SECTION("initial state is idle without surfacing historical info")
    {
      feedState.initialize(feed({entry(rt::NotificationId{1}, rt::NotificationSeverity::Info, "Saved playlist")}));

      CHECK(feedState.viewState().compact.kind == ActivityStatusKind::Idle);
      CHECK(feedState.viewState().detail.items.empty());
      CHECK_FALSE(hasDetailContent(feedState.viewState().detail));
    }

    SECTION("library progress owns the compact readout")
    {
      feedState.handleLibraryTaskProgress("Scanning: long-file-name.flac", 0.625);

      auto const& compact = feedState.viewState().compact;
      CHECK(compact.kind == ActivityStatusKind::Processing);
      CHECK(compact.text == "Scanning library");
      CHECK(compact.optProgressFraction == 0.625);
      CHECK(!compact.optAutoDismissTimeout);
    }

    SECTION("library completion is a transient success")
    {
      feedState.handleLibraryTaskProgress("Updating: track.flac", 0.8);
      feedState.handleLibraryTaskCompleted(17, feed({}));

      auto const& compact = feedState.viewState().compact;
      CHECK(compact.kind == ActivityStatusKind::Success);
      CHECK(compact.text == "Scan complete: 17 tracks added");
      CHECK(compact.optAutoDismissTimeout == kActivityStatusDefaultAutoDismissTimeout);
    }

    SECTION("notification during task is deferred and errors beat completion")
    {
      feedState.handleLibraryTaskProgress("Scanning: album.flac", 0.4);

      auto const error = entry(rt::NotificationId{4}, rt::NotificationSeverity::Error, "Import failed", true);
      auto const currentFeed = feed({error});
      feedState.handleNotificationPosted(currentFeed, rt::NotificationId{4});
      CHECK(feedState.viewState().compact.kind == ActivityStatusKind::Processing);

      feedState.handleLibraryTaskCompleted(9, currentFeed);

      auto const& compact = feedState.viewState().compact;
      CHECK(compact.kind == ActivityStatusKind::Error);
      CHECK(compact.text == "Import failed");
      CHECK(compact.persistent);
      CHECK(!compact.optAutoDismissTimeout);
    }

    SECTION("deferred persistent notification is ignored when removed before task completion")
    {
      feedState.handleLibraryTaskProgress("Scanning: album.flac", 0.4);

      auto const error = entry(rt::NotificationId{15}, rt::NotificationSeverity::Error, "Import failed", true);
      feedState.handleNotificationPosted(feed({error}), rt::NotificationId{15});

      feedState.handleLibraryTaskCompleted(9, feed({}));

      CHECK(feedState.viewState().compact.kind == ActivityStatusKind::Success);
      CHECK(feedState.viewState().compact.text == "Scan complete: 9 tracks added");
      CHECK(feedState.viewState().detail.items.empty());
    }

    SECTION("success and info do not override persistent warnings")
    {
      auto warningFeed = feed({entry(rt::NotificationId{2}, rt::NotificationSeverity::Warning, "Partial import")});
      feedState.handleNotificationPosted(warningFeed, rt::NotificationId{2});

      auto infoFeed = feed({entry(rt::NotificationId{2}, rt::NotificationSeverity::Warning, "Partial import"),
                            entry(rt::NotificationId{3}, rt::NotificationSeverity::Info, "Saved playlist")});
      feedState.handleNotificationPosted(infoFeed, rt::NotificationId{3});

      CHECK(feedState.viewState().compact.kind == ActivityStatusKind::Warning);
      CHECK(feedState.viewState().compact.text == "Partial import");
    }
  }
} // namespace ao::uimodel::test
