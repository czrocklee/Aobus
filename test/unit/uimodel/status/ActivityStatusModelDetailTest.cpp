// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/uimodel/status/ActivityStatusModelTestSupport.h"
#include <ao/rt/StateTypes.h>
#include <ao/uimodel/status/ActivityStatusModel.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::uimodel::status::test
{
  TEST_CASE("ActivityStatusModel projects detail feed items and helpers", "[uimodel][unit][status][activity]")
  {
    auto model = ActivityStatusModel{};
    auto notification = entry(rt::NotificationId{12}, rt::NotificationSeverity::Error, "Write failed", true);
    notification.content.title = "Library Error";
    notification.content.iconName = "dialog-error-symbolic";
    notification.content.actions = {rt::NotificationAction{.id = "library.retry", .label = "Retry"}};

    auto currentFeed = feed({entry(rt::NotificationId{13}, rt::NotificationSeverity::Warning, "Clearable warning"),
                             notification,
                             progressEntry(rt::NotificationId{14}, "Importing", 0.25)});

    model.initialize(currentFeed);

    SECTION("detail items keep progress first and latest-first ordering")
    {
      auto const& detail = model.viewState().detail;
      REQUIRE(detail.items.size() == 3);
      CHECK(detail.hasActiveProgress);
      CHECK(detail.items[0].id == rt::NotificationId{14});
      CHECK(detail.items[0].optProgressMode == rt::NotificationProgressMode::Fraction);
      CHECK(detail.items[0].progressFraction == 0.25);
      CHECK(detail.items[1].id == rt::NotificationId{13});
      CHECK(detail.items[2].id == rt::NotificationId{12});
      CHECK(hasDetailContent(detail));
    }

    SECTION("synthetic library progress is retained as task detail")
    {
      model.onLibraryTaskProgress("Scanning: album.flac", 0.5);

      auto const& detail = model.viewState().detail;
      REQUIRE(detail.optLibraryTask);
      CHECK(detail.optLibraryTask->message == "Scanning: album.flac");
      CHECK(detail.optLibraryTask->progressFraction == 0.5);
      CHECK(detail.hasActiveProgress);
    }

    SECTION("detail items expose title icon sticky severity and actions")
    {
      auto const& item = model.viewState().detail.items[2];

      CHECK(item.severity == rt::NotificationSeverity::Error);
      CHECK(item.title == "Library Error");
      CHECK(item.message == "Write failed");
      CHECK(item.iconName == "dialog-error-symbolic");
      CHECK(item.sticky);
      CHECK_FALSE(item.dismissible);
      REQUIRE(item.actions.size() == 1);
      CHECK(item.actions[0].id == "library.retry");
      CHECK(item.actions[0].label == "Retry");
    }

    SECTION("clearable ids skip sticky and progress notifications")
    {
      auto const ids = model.locallyHideableNotificationIds(currentFeed);

      REQUIRE(ids.size() == 1);
      CHECK(ids[0] == rt::NotificationId{13});
    }

    SECTION("detail dismiss ignores sticky and progress notifications")
    {
      model.hideDetailNotificationFromActivity(rt::NotificationId{12}, currentFeed);
      model.hideDetailNotificationFromActivity(rt::NotificationId{14}, currentFeed);

      REQUIRE(model.viewState().detail.items.size() == 3);
      CHECK(model.viewState().detail.items[0].id == rt::NotificationId{14});
      CHECK(model.viewState().detail.items[2].id == rt::NotificationId{12});
    }

    SECTION("rich default info remains detail eligible")
    {
      auto info = entry(rt::NotificationId{22}, rt::NotificationSeverity::Info, "Playlist saved");
      info.content.title = "Playlist";
      info.content.actions = {rt::NotificationAction{.id = "playlist.open", .label = "Open"}};

      auto const infoFeed = feed({info});
      model.initialize(infoFeed);

      auto const& detail = model.viewState().detail;
      REQUIRE(detail.items.size() == 1);
      CHECK(detail.items[0].id == rt::NotificationId{22});
      CHECK(detail.items[0].title == "Playlist");
      REQUIRE(detail.items[0].actions.size() == 1);
      CHECK(detail.items[0].actions[0].id == "playlist.open");
      CHECK(hasDetailContent(detail));
    }

    SECTION("indeterminate progress mode is preserved for detail rendering")
    {
      auto progress = entry(rt::NotificationId{30}, rt::NotificationSeverity::Info, "Importing");
      progress.content.optProgress = rt::NotificationProgressState{
        .mode = rt::NotificationProgressMode::Indeterminate, .fraction = 0.0, .label = "Importing"};

      model.initialize(feed({progress}));

      auto const& detail = model.viewState().detail;
      REQUIRE(detail.items.size() == 1);
      CHECK(detail.items[0].optProgressMode == rt::NotificationProgressMode::Indeterminate);
      CHECK(detail.hasActiveProgress);
    }

    SECTION("kind css mapping remains explicit")
    {
      CHECK(activityStatusKindCssClass(ActivityStatusKind::Idle) == "ao-activity-status-idle");
      CHECK(activityStatusKindCssClass(ActivityStatusKind::Processing) == "ao-activity-status-processing");
      CHECK(activityStatusKindCssClass(ActivityStatusKind::Success) == "ao-activity-status-success");
      CHECK(activityStatusKindCssClass(ActivityStatusKind::Info) == "ao-activity-status-info");
      CHECK(activityStatusKindCssClass(ActivityStatusKind::Warning) == "ao-activity-status-warning");
      CHECK(activityStatusKindCssClass(ActivityStatusKind::Error) == "ao-activity-status-error");
    }
  }
} // namespace ao::uimodel::status::test
