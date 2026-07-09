// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/uimodel/status/activity/ActivityStatusFeedProjectionTestSupport.h"
#include "uimodel/status/activity/ActivityStatusFeedProjection.h"
#include <ao/rt/NotificationState.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::uimodel::test
{
  TEST_CASE("ActivityStatusFeedProjection - projects compact state from runtime priority",
            "[uimodel][unit][status][activity]")
  {
    auto feedProjection = ActivityStatusFeedProjection{};

    SECTION("initial state is idle without surfacing historical info")
    {
      feedProjection.initialize(feed({entry(rt::NotificationId{1}, rt::NotificationSeverity::Info, "Saved playlist")}));

      CHECK(feedProjection.viewState().compact.kind == ActivityStatusKind::Idle);
      CHECK(feedProjection.viewState().detail.items.empty());
      CHECK_FALSE(hasDetailContent(feedProjection.viewState().detail));
    }

    SECTION("library progress owns the compact readout")
    {
      feedProjection.handleLibraryTaskProgress("Scanning: long-file-name.flac", 0.625);

      auto const& compact = feedProjection.viewState().compact;
      CHECK(compact.kind == ActivityStatusKind::Processing);
      CHECK(compact.text == "Scanning library");
      CHECK(compact.optProgressFraction == 0.625);
      CHECK(!compact.optAutoDismissTimeout);
    }

    SECTION("library completion is a transient success")
    {
      feedProjection.handleLibraryTaskProgress("Updating: track.flac", 0.8);
      feedProjection.handleLibraryTaskCompleted(17, feed({}));

      auto const& compact = feedProjection.viewState().compact;
      CHECK(compact.kind == ActivityStatusKind::Success);
      CHECK(compact.text == "Scan complete: 17 tracks added");
      CHECK(compact.optAutoDismissTimeout == kActivityStatusDefaultAutoDismissTimeout);
    }

    SECTION("notification during task is deferred and errors beat completion")
    {
      feedProjection.handleLibraryTaskProgress("Scanning: album.flac", 0.4);

      auto const error = entry(rt::NotificationId{4}, rt::NotificationSeverity::Error, "Import failed", true);
      auto const currentFeed = feed({error});
      feedProjection.handleNotificationPosted(currentFeed, rt::NotificationId{4});
      CHECK(feedProjection.viewState().compact.kind == ActivityStatusKind::Processing);

      feedProjection.handleLibraryTaskCompleted(9, currentFeed);

      auto const& compact = feedProjection.viewState().compact;
      CHECK(compact.kind == ActivityStatusKind::Error);
      CHECK(compact.text == "Import failed");
      CHECK(compact.persistent);
      CHECK(!compact.optAutoDismissTimeout);
    }

    SECTION("deferred persistent notification is ignored when removed before task completion")
    {
      feedProjection.handleLibraryTaskProgress("Scanning: album.flac", 0.4);

      auto const error = entry(rt::NotificationId{15}, rt::NotificationSeverity::Error, "Import failed", true);
      feedProjection.handleNotificationPosted(feed({error}), rt::NotificationId{15});

      feedProjection.handleLibraryTaskCompleted(9, feed({}));

      CHECK(feedProjection.viewState().compact.kind == ActivityStatusKind::Success);
      CHECK(feedProjection.viewState().compact.text == "Scan complete: 9 tracks added");
      CHECK(feedProjection.viewState().detail.items.empty());
    }

    SECTION("success and info do not override persistent warnings")
    {
      auto warningFeed = feed({entry(rt::NotificationId{2}, rt::NotificationSeverity::Warning, "Partial import")});
      feedProjection.handleNotificationPosted(warningFeed, rt::NotificationId{2});

      auto infoFeed = feed({entry(rt::NotificationId{2}, rt::NotificationSeverity::Warning, "Partial import"),
                            entry(rt::NotificationId{3}, rt::NotificationSeverity::Info, "Saved playlist")});
      feedProjection.handleNotificationPosted(infoFeed, rt::NotificationId{3});

      CHECK(feedProjection.viewState().compact.kind == ActivityStatusKind::Warning);
      CHECK(feedProjection.viewState().compact.text == "Partial import");
    }
  }
} // namespace ao::uimodel::test
