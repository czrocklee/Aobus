// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/uimodel/status/activity/ActivityStatusFeedProjectionTestSupport.h"
#include "uimodel/status/activity/ActivityStatusFeedProjection.h"
#include <ao/rt/NotificationState.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::uimodel::test
{
  TEST_CASE("ActivityStatusFeedProjection - projects detail feed items and helpers",
            "[uimodel][unit][status][activity]")
  {
    auto feedProjection = ActivityStatusFeedProjection{};
    auto notification = entry(rt::NotificationId{12}, rt::NotificationSeverity::Error, "Write failed", true);
    notification.content.title = "Library Error";
    notification.content.iconName = "dialog-error-symbolic";
    notification.content.actions = {rt::NotificationAction{.id = "library.retry", .label = "Retry"}};

    auto currentFeed = feed({entry(rt::NotificationId{13}, rt::NotificationSeverity::Warning, "Clearable warning"),
                             notification,
                             progressEntry(rt::NotificationId{14}, "Importing", 0.25)});

    feedProjection.initialize(currentFeed);

    SECTION("detail items keep progress first and latest-first ordering")
    {
      auto const& detail = feedProjection.viewState().detail;
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
      feedProjection.handleLibraryTaskProgress("Scanning: album.flac", 0.5);

      auto const& detail = feedProjection.viewState().detail;
      REQUIRE(detail.optLibraryTask);
      CHECK(detail.optLibraryTask->message == "Scanning: album.flac");
      CHECK(detail.optLibraryTask->progressFraction == 0.5);
      CHECK(detail.hasActiveProgress);
    }

    SECTION("detail items expose title icon sticky severity and actions")
    {
      auto const& item = feedProjection.viewState().detail.items[2];

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
      auto const ids = feedProjection.locallyHideableNotificationIds(currentFeed);

      REQUIRE(ids.size() == 1);
      CHECK(ids[0] == rt::NotificationId{13});
    }

    SECTION("detail dismiss ignores sticky and progress notifications")
    {
      feedProjection.dismissDetailNotificationFromActivity(rt::NotificationId{12}, currentFeed);
      feedProjection.dismissDetailNotificationFromActivity(rt::NotificationId{14}, currentFeed);

      REQUIRE(feedProjection.viewState().detail.items.size() == 3);
      CHECK(feedProjection.viewState().detail.items[0].id == rt::NotificationId{14});
      CHECK(feedProjection.viewState().detail.items[2].id == rt::NotificationId{12});
    }

    SECTION("rich default info remains detail eligible")
    {
      auto info = entry(rt::NotificationId{22}, rt::NotificationSeverity::Info, "Playlist saved");
      info.content.title = "Playlist";
      info.content.actions = {rt::NotificationAction{.id = "playlist.open", .label = "Open"}};

      auto const infoFeed = feed({info});
      feedProjection.initialize(infoFeed);

      auto const& detail = feedProjection.viewState().detail;
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

      feedProjection.initialize(feed({progress}));

      auto const& detail = feedProjection.viewState().detail;
      REQUIRE(detail.items.size() == 1);
      CHECK(detail.items[0].optProgressMode == rt::NotificationProgressMode::Indeterminate);
      CHECK(detail.hasActiveProgress);
    }

    SECTION("action render policy filters hidden actions and keeps disabled reasons")
    {
      auto const actions = std::vector<ActivityActionDescriptor>{
        {.id = "library.retry", .label = "Retry"}, {.id = "library.ignore", .label = "Ignore"}};

      auto const resolved = resolveActivityActionStates(
        actions,
        [](std::string_view actionId, std::string_view label)
        {
          if (actionId == "library.retry")
          {
            return ActivityActionAvailability{
              .visible = true, .enabled = false, .label = std::string{label}, .disabledReason = "Library busy"};
          }

          return ActivityActionAvailability{};
        },
        2);

      REQUIRE(resolved.size() == 1);
      CHECK(resolved[0].id == "library.retry");
      CHECK(resolved[0].label == "Retry");
      CHECK(resolved[0].enabled == false);
      CHECK(resolved[0].disabledReason == "Library busy");
    }

    SECTION("action render policy skips empty labels and caps visible actions")
    {
      auto const actions = std::vector<ActivityActionDescriptor>{{.id = "library.retry", .label = ""},
                                                                 {.id = "library.ignore", .label = "Ignore"},
                                                                 {.id = "library.details", .label = "Details"}};

      auto const resolved = resolveActivityActionStates(
        actions,
        [](std::string_view actionId, std::string_view label)
        {
          auto resolvedLabel = std::string{label};

          if (actionId == "library.retry")
          {
            resolvedLabel = "Retry";
          }

          return ActivityActionAvailability{.visible = true, .enabled = true, .label = std::move(resolvedLabel)};
        },
        2);

      REQUIRE(resolved.size() == 2);
      CHECK(resolved[0].id == "library.retry");
      CHECK(resolved[0].label == "Retry");
      CHECK(resolved[1].id == "library.ignore");
      CHECK(resolved[1].label == "Ignore");
    }
  }
} // namespace ao::uimodel::test
