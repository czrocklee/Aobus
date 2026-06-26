// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/CorePrimitives.h>
#include <ao/rt/StateTypes.h>
#include <ao/uimodel/status/ActivityStatusModel.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ao::uimodel::status::test
{
  namespace
  {
    rt::NotificationEntry entry(
      rt::NotificationId id,
      rt::NotificationSeverity severity,
      std::string message,
      bool sticky = false,
      std::optional<std::chrono::milliseconds> optTimeout = std::nullopt,
      rt::NotificationActivityPresentation activityPresentation = rt::NotificationActivityPresentation::Default)
    {
      return rt::NotificationEntry{
        .id = id,
        .severity = severity,
        .message = std::move(message),
        .sticky = sticky,
        .optTimeout = optTimeout,
        .activityPresentation = activityPresentation,
      };
    }

    rt::NotificationEntry progressEntry(rt::NotificationId id, std::string message, double fraction)
    {
      auto result = entry(id, rt::NotificationSeverity::Info, std::move(message));
      result.content.optProgress = rt::NotificationProgressState{
        .mode = rt::NotificationProgressMode::Fraction, .fraction = fraction, .label = "Importing"};
      return result;
    }

    rt::NotificationFeedState feed(std::vector<rt::NotificationEntry> entries)
    {
      return rt::NotificationFeedState{.entries = std::move(entries), .revision = 9};
    }
  } // namespace

  TEST_CASE("ActivityStatusModel - compact state follows runtime priority", "[uimodel][status][activity]")
  {
    auto model = ActivityStatusModel{};

    SECTION("initial state is idle without surfacing historical info")
    {
      model.initialize(feed({entry(rt::NotificationId{1}, rt::NotificationSeverity::Info, "Saved playlist")}));

      CHECK(model.viewState().compact.kind == ActivityStatusKind::Idle);
      CHECK(model.viewState().detail.items.empty());
      CHECK_FALSE(hasDetailContent(model.viewState().detail));
    }

    SECTION("library progress owns the compact readout")
    {
      model.onLibraryTaskProgress("Scanning: long-file-name.flac", 0.625);

      auto const& compact = model.viewState().compact;
      CHECK(compact.kind == ActivityStatusKind::Processing);
      CHECK(compact.text == "Scanning library");
      CHECK(compact.optProgressFraction == 0.625);
      CHECK(!compact.optAutoDismissTimeout);
    }

    SECTION("library completion is a transient success")
    {
      model.onLibraryTaskProgress("Updating: track.flac", 0.8);
      model.onLibraryTaskCompleted(17, feed({}));

      auto const& compact = model.viewState().compact;
      CHECK(compact.kind == ActivityStatusKind::Success);
      CHECK(compact.text == "Scan complete: 17 tracks added");
      CHECK(compact.optAutoDismissTimeout == kActivityStatusDefaultAutoDismissTimeout);
    }

    SECTION("notification during task is deferred and errors beat completion")
    {
      model.onLibraryTaskProgress("Scanning: album.flac", 0.4);

      auto const error = entry(rt::NotificationId{4}, rt::NotificationSeverity::Error, "Import failed", true);
      auto const currentFeed = feed({error});
      model.onNotificationPosted(currentFeed, rt::NotificationId{4});
      CHECK(model.viewState().compact.kind == ActivityStatusKind::Processing);

      model.onLibraryTaskCompleted(9, currentFeed);

      auto const& compact = model.viewState().compact;
      CHECK(compact.kind == ActivityStatusKind::Error);
      CHECK(compact.text == "Import failed");
      CHECK(compact.persistent);
      CHECK(!compact.optAutoDismissTimeout);
    }

    SECTION("deferred persistent notification is ignored when removed before task completion")
    {
      model.onLibraryTaskProgress("Scanning: album.flac", 0.4);

      auto const error = entry(rt::NotificationId{15}, rt::NotificationSeverity::Error, "Import failed", true);
      model.onNotificationPosted(feed({error}), rt::NotificationId{15});

      model.onLibraryTaskCompleted(9, feed({}));

      CHECK(model.viewState().compact.kind == ActivityStatusKind::Success);
      CHECK(model.viewState().compact.text == "Scan complete: 9 tracks added");
      CHECK(model.viewState().detail.items.empty());
    }

    SECTION("success and info do not override persistent warnings")
    {
      auto warningFeed = feed({entry(rt::NotificationId{2}, rt::NotificationSeverity::Warning, "Partial import")});
      model.onNotificationPosted(warningFeed, rt::NotificationId{2});

      auto infoFeed = feed({entry(rt::NotificationId{2}, rt::NotificationSeverity::Warning, "Partial import"),
                            entry(rt::NotificationId{3}, rt::NotificationSeverity::Info, "Saved playlist")});
      model.onNotificationPosted(infoFeed, rt::NotificationId{3});

      CHECK(model.viewState().compact.kind == ActivityStatusKind::Warning);
      CHECK(model.viewState().compact.text == "Partial import");
    }
  }

  TEST_CASE("ActivityStatusModel - notification projection and dismissal", "[uimodel][status][activity]")
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

    SECTION("hidden notification presentation is ignored by activity status")
    {
      auto currentFeed = feed({entry(rt::NotificationId{11},
                                     rt::NotificationSeverity::Info,
                                     "Aobus Ready",
                                     false,
                                     std::nullopt,
                                     rt::NotificationActivityPresentation::Hidden)});

      model.onNotificationPosted(currentFeed, rt::NotificationId{11});

      CHECK(model.viewState().compact.kind == ActivityStatusKind::Idle);
      CHECK(model.viewState().detail.items.empty());
    }

    SECTION("detail-only notification presentation does not create compact state")
    {
      auto currentFeed = feed({entry(rt::NotificationId{19},
                                     rt::NotificationSeverity::Info,
                                     "Index diagnostic",
                                     false,
                                     std::nullopt,
                                     rt::NotificationActivityPresentation::DetailOnly)});

      model.onNotificationPosted(currentFeed, rt::NotificationId{19});

      CHECK(model.viewState().compact.kind == ActivityStatusKind::Idle);
      REQUIRE(model.viewState().detail.items.size() == 1);
      CHECK(model.viewState().detail.items[0].message == "Index diagnostic");
      CHECK(hasDetailContent(model.viewState().detail));
    }

    SECTION("non-compact presentations do not interrupt library progress")
    {
      model.onLibraryTaskProgress("Scanning: album.flac", 0.4);

      auto currentFeed = feed({entry(rt::NotificationId{25},
                                     rt::NotificationSeverity::Info,
                                     "Index diagnostic",
                                     false,
                                     std::nullopt,
                                     rt::NotificationActivityPresentation::DetailOnly)});
      model.onNotificationPosted(currentFeed, rt::NotificationId{25});

      CHECK(model.viewState().compact.kind == ActivityStatusKind::Processing);
      CHECK(model.viewState().compact.text == "Scanning library");
      REQUIRE(model.viewState().detail.items.size() == 1);
      CHECK(model.viewState().detail.items[0].message == "Index diagnostic");

      model.onLibraryTaskCompleted(3, currentFeed);

      CHECK(model.viewState().compact.kind == ActivityStatusKind::Success);
      CHECK(model.viewState().compact.text == "Scan complete: 3 tracks added");
    }

    SECTION("hidden presentation does not interrupt library progress")
    {
      model.onLibraryTaskProgress("Updating: album.flac", 0.7);

      auto currentFeed = feed({entry(rt::NotificationId{26},
                                     rt::NotificationSeverity::Info,
                                     "Aobus Ready",
                                     false,
                                     std::nullopt,
                                     rt::NotificationActivityPresentation::Hidden)});
      model.onNotificationPosted(currentFeed, rt::NotificationId{26});

      CHECK(model.viewState().compact.kind == ActivityStatusKind::Processing);
      CHECK(model.viewState().compact.text == "Updating library");
      CHECK(model.viewState().detail.items.empty());
    }

    SECTION("non-compact presentations do not replace transient compact state")
    {
      auto info = entry(rt::NotificationId{27}, rt::NotificationSeverity::Info, "Saved playlist");
      model.onNotificationPosted(feed({info}), rt::NotificationId{27});
      REQUIRE(model.viewState().compact.kind == ActivityStatusKind::Info);

      auto detailOnly = entry(rt::NotificationId{28},
                              rt::NotificationSeverity::Info,
                              "Index diagnostic",
                              false,
                              std::nullopt,
                              rt::NotificationActivityPresentation::DetailOnly);
      model.onNotificationPosted(feed({info, detailOnly}), rt::NotificationId{28});

      CHECK(model.viewState().compact.kind == ActivityStatusKind::Info);
      CHECK(model.viewState().compact.text == "Saved playlist");
      REQUIRE(model.viewState().detail.items.size() == 1);
      CHECK(model.viewState().detail.items[0].id == rt::NotificationId{28});
    }

    SECTION("default notification presentation remains compact eligible")
    {
      auto currentFeed = feed({entry(rt::NotificationId{20}, rt::NotificationSeverity::Warning, "Default warning")});

      model.onNotificationPosted(currentFeed, rt::NotificationId{20});

      CHECK(model.viewState().compact.kind == ActivityStatusKind::Warning);
      CHECK(model.viewState().compact.text == "Default warning");
      REQUIRE(model.viewState().detail.items.size() == 1);
    }

    SECTION("plain default info creates compact state without openable detail")
    {
      auto currentFeed = feed({entry(rt::NotificationId{21}, rt::NotificationSeverity::Info, "Saved playlist")});

      model.onNotificationPosted(currentFeed, rt::NotificationId{21});

      CHECK(model.viewState().compact.kind == ActivityStatusKind::Info);
      CHECK(model.viewState().compact.text == "Saved playlist");
      CHECK_FALSE(model.viewState().compact.hasDetails);
      CHECK(model.viewState().detail.items.empty());
      CHECK_FALSE(hasDetailContent(model.viewState().detail));
    }

    SECTION("sticky info remains transient compact while staying detail eligible")
    {
      auto currentFeed = feed({entry(rt::NotificationId{29}, rt::NotificationSeverity::Info, "Background note", true)});

      model.onNotificationPosted(currentFeed, rt::NotificationId{29});

      CHECK(model.viewState().compact.kind == ActivityStatusKind::Info);
      CHECK(model.viewState().compact.text == "Background note");
      CHECK_FALSE(model.viewState().compact.persistent);
      CHECK(model.viewState().compact.optAutoDismissTimeout == kActivityStatusDefaultAutoDismissTimeout);
      REQUIRE(model.viewState().detail.items.size() == 1);
      CHECK(model.viewState().detail.items[0].sticky);
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

  TEST_CASE("ActivityStatusModel - detail feed projection", "[uimodel][status][activity]")
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
