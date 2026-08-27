// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/uimodel/status/activity/ActivityStatusFeedProjectionTestSupport.h"
#include "uimodel/status/activity/ActivityStatusFeedProjection.h"
#include <ao/rt/NotificationState.h>
#include <ao/rt/library/LibraryTaskEvents.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::uimodel::test
{
  TEST_CASE("ActivityStatusFeedProjection - projects compact state from runtime priority",
            "[uimodel][unit][status][activity]")
  {
    auto feedProjection = ActivityStatusFeedProjection{ao::test::englishMessageCatalog(), feed({})};

    SECTION("initial state is idle without surfacing historical info")
    {
      feedProjection = ActivityStatusFeedProjection{
        ao::test::englishMessageCatalog(),
        feed({entry(rt::NotificationId{1}, rt::NotificationSeverity::Info, "Saved playlist")})};

      CHECK(feedProjection.viewState().compact.kind == ActivityStatusKind::Idle);
      CHECK(feedProjection.viewState().detail.items.empty());
      CHECK_FALSE(hasDetailContent(feedProjection.viewState().detail));
    }

    SECTION("library progress owns the compact readout")
    {
      feedProjection.handleLibraryTaskProgress(
        libraryTaskProgress(rt::LibraryTaskProgressKind::Scanning, "long-file-name.flac", 0.625));

      auto const& compact = feedProjection.viewState().compact;
      CHECK(compact.kind == ActivityStatusKind::Processing);
      CHECK(compact.text == "Scanning library");
      CHECK(compact.optProgressFraction == 0.625);
      CHECK(!compact.optAutoDismissTimeout);
    }

    SECTION("progress finish clears the task without synthesizing an outcome")
    {
      feedProjection.handleLibraryTaskProgress(
        libraryTaskProgress(rt::LibraryTaskProgressKind::Updating, "track.flac", 0.8));
      feedProjection.handleLibraryProgressFinished(libraryTaskProgressFinished(), feed({}));

      auto const& compact = feedProjection.viewState().compact;
      CHECK(compact.kind == ActivityStatusKind::Idle);
      CHECK(compact.text.empty());
      CHECK_FALSE(compact.optAutoDismissTimeout);
      CHECK_FALSE(feedProjection.viewState().detail.optLibraryTask);
    }

    SECTION("notification during task is deferred and errors appear when progress finishes")
    {
      feedProjection.handleLibraryTaskProgress(
        libraryTaskProgress(rt::LibraryTaskProgressKind::Scanning, "album.flac", 0.4));

      auto const error = entry(
        rt::NotificationId{4}, rt::NotificationSeverity::Error, "Import failed", rt::NotificationLifetime::pinned());
      auto const currentFeed = feed({error});
      feedProjection.handleFeedUpdated(postedUpdate(currentFeed, rt::NotificationId{4}));
      CHECK(feedProjection.viewState().compact.kind == ActivityStatusKind::Processing);

      feedProjection.handleLibraryProgressFinished(libraryTaskProgressFinished(), currentFeed);

      auto const& compact = feedProjection.viewState().compact;
      CHECK(compact.kind == ActivityStatusKind::Error);
      CHECK(compact.text == "Import failed");
      CHECK(!compact.optAutoDismissTimeout);
    }

    SECTION("deferred persistent notification is ignored when removed before progress finishes")
    {
      feedProjection.handleLibraryTaskProgress(
        libraryTaskProgress(rt::LibraryTaskProgressKind::Scanning, "album.flac", 0.4));

      auto const error = entry(
        rt::NotificationId{15}, rt::NotificationSeverity::Error, "Import failed", rt::NotificationLifetime::pinned());
      feedProjection.handleFeedUpdated(postedUpdate(feed({error}), rt::NotificationId{15}));

      feedProjection.handleLibraryProgressFinished(libraryTaskProgressFinished(), feed({}));

      CHECK(feedProjection.viewState().compact.kind == ActivityStatusKind::Idle);
      CHECK(feedProjection.viewState().detail.items.empty());
    }

    SECTION("finished pulse carries no terminal status")
    {
      feedProjection.handleLibraryTaskProgress(
        libraryTaskProgress(rt::LibraryTaskProgressKind::Scanning, "album.flac", 0.4));
      feedProjection.handleLibraryProgressFinished(libraryTaskProgressFinished(), feed({}));

      CHECK(feedProjection.viewState().compact.kind == ActivityStatusKind::Idle);
      CHECK_FALSE(feedProjection.viewState().detail.optLibraryTask);
    }

    SECTION("info does not override persistent warnings")
    {
      auto warningFeed = feed({entry(rt::NotificationId{2}, rt::NotificationSeverity::Warning, "Partial import")});
      feedProjection.handleFeedUpdated(postedUpdate(warningFeed, rt::NotificationId{2}));

      auto infoFeed = feed({entry(rt::NotificationId{2}, rt::NotificationSeverity::Warning, "Partial import"),
                            entry(rt::NotificationId{3}, rt::NotificationSeverity::Info, "Saved playlist")});
      feedProjection.handleFeedUpdated(postedUpdate(infoFeed, rt::NotificationId{3}));

      CHECK(feedProjection.viewState().compact.kind == ActivityStatusKind::Warning);
      CHECK(feedProjection.viewState().compact.text == "Partial import");
    }
  }

  TEST_CASE("ActivityStatusFeedProjection - finishing an overlapping export restores active scan progress",
            "[uimodel][regression][activity-status][concurrency]")
  {
    auto feedProjection = ActivityStatusFeedProjection{ao::test::englishMessageCatalog(), feed({})};
    auto const scanId = rt::LibraryTaskProgressId{11};
    auto const exportId = rt::LibraryTaskProgressId{12};
    feedProjection.handleLibraryTaskProgress(
      libraryTaskProgress(rt::LibraryTaskProgressKind::Scanning, "album.flac", 0.4, scanId));
    feedProjection.handleLibraryTaskProgress(
      libraryTaskProgress(rt::LibraryTaskProgressKind::Exporting, "backup.yaml", 0.0, exportId));

    REQUIRE(feedProjection.viewState().compact.kind == ActivityStatusKind::Processing);
    CHECK(feedProjection.viewState().compact.text == "Exporting: backup.yaml");

    feedProjection.handleLibraryProgressFinished(libraryTaskProgressFinished(exportId), feed({}));

    CHECK(feedProjection.viewState().compact.kind == ActivityStatusKind::Processing);
    CHECK(feedProjection.viewState().compact.text == "Scanning library");
    CHECK(feedProjection.viewState().compact.optProgressFraction == 0.4);
    REQUIRE(feedProjection.viewState().detail.optLibraryTask);
    CHECK(feedProjection.viewState().detail.optLibraryTask->progressFraction == 0.4);

    feedProjection.handleLibraryProgressFinished(libraryTaskProgressFinished(scanId), feed({}));

    CHECK(feedProjection.viewState().compact.kind == ActivityStatusKind::Idle);
    CHECK_FALSE(feedProjection.viewState().detail.optLibraryTask);
  }
} // namespace ao::uimodel::test
