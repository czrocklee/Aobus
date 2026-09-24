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
  TEST_CASE("ActivityStatusFeedProjection - projects detail feed items and helpers", "[uimodel][unit][activity-status]")
  {
    auto const retainedError = entry(
      rt::NotificationId{12}, rt::NotificationSeverity::Error, "Write failed", rt::NotificationLifetime::pinned());
    auto const clearableWarning = entry(rt::NotificationId{13}, rt::NotificationSeverity::Warning, "Clearable warning");
    auto const latestWarning = entry(rt::NotificationId{14}, rt::NotificationSeverity::Warning, "Latest warning");
    auto currentFeed = feed({clearableWarning, retainedError, latestWarning});
    auto feedProjection = ActivityStatusFeedProjection{ao::test::englishMessageCatalog(), currentFeed};

    SECTION("detail items use latest-first ordering")
    {
      auto const& detail = feedProjection.viewState().detail;
      REQUIRE(detail.items.size() == 3);
      CHECK(detail.items[0].id == rt::NotificationId{14});
      CHECK(detail.items[1].id == rt::NotificationId{13});
      CHECK(detail.items[2].id == rt::NotificationId{12});
      CHECK(hasDetailContent(detail));
    }

    SECTION("synthetic library progress is retained as task detail")
    {
      feedProjection.handleLibraryTaskProgress(
        libraryTaskProgress(rt::LibraryTaskProgressKind::Scanning, "album.flac", 0.5));

      auto const& detail = feedProjection.viewState().detail;
      REQUIRE(detail.optLibraryTask);
      CHECK(detail.optLibraryTask->message == "Scanning: album.flac");
      CHECK(detail.optLibraryTask->progressFraction == 0.5);
    }

    SECTION("detail items expose severity message and local dismissibility")
    {
      auto const& detail = feedProjection.viewState().detail;
      REQUIRE(detail.items.size() == 3);
      auto const& errorItem = detail.items[2];
      CHECK(errorItem.id == rt::NotificationId{12});
      CHECK(errorItem.severity == rt::NotificationSeverity::Error);
      CHECK(errorItem.message == "Write failed");
      CHECK_FALSE(errorItem.dismissible);

      auto const& warningItem = detail.items[0];
      CHECK(warningItem.id == rt::NotificationId{14});
      CHECK(warningItem.severity == rt::NotificationSeverity::Warning);
      CHECK(warningItem.message == "Latest warning");
      CHECK(warningItem.dismissible);

      auto const& clearableItem = detail.items[1];
      CHECK(clearableItem.id == rt::NotificationId{13});
      CHECK(clearableItem.severity == rt::NotificationSeverity::Warning);
      CHECK(clearableItem.message == "Clearable warning");
      CHECK(clearableItem.dismissible);
    }

    SECTION("detail dismiss ignores pinned and hides clearable notifications")
    {
      feedProjection.hideDetailNotification(rt::NotificationId{12}, currentFeed);
      REQUIRE(feedProjection.viewState().detail.items.size() == 3);
      CHECK(feedProjection.viewState().detail.items[0].id == rt::NotificationId{14});
      CHECK(feedProjection.viewState().detail.items[1].id == rt::NotificationId{13});
      CHECK(feedProjection.viewState().detail.items[2].id == rt::NotificationId{12});

      feedProjection.hideDetailNotification(rt::NotificationId{14}, currentFeed);

      REQUIRE(feedProjection.viewState().detail.items.size() == 2);
      CHECK(feedProjection.viewState().detail.items[0].id == rt::NotificationId{13});
      CHECK(feedProjection.viewState().detail.items[1].id == rt::NotificationId{12});
    }
  }

  TEST_CASE("ActivityStatusFeedProjection - task-only detail counts as content", "[uimodel][unit][activity-status]")
  {
    auto feedProjection = ActivityStatusFeedProjection{ao::test::englishMessageCatalog(), feed({})};
    REQUIRE_FALSE(hasDetailContent(feedProjection.viewState().detail));

    feedProjection.handleLibraryTaskProgress(
      libraryTaskProgress(rt::LibraryTaskProgressKind::Scanning, "album.flac", 0.5));

    auto const& detail = feedProjection.viewState().detail;
    REQUIRE(detail.items.empty());
    REQUIRE(detail.optLibraryTask);
    CHECK(detail.optLibraryTask->message == "Scanning: album.flac");
    CHECK(detail.optLibraryTask->progressFraction == 0.5);
    CHECK(hasDetailContent(detail));
  }
} // namespace ao::uimodel::test
