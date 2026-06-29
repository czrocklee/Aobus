// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/uimodel/status/activity/ActivityStatusModelTestSupport.h"
#include <ao/rt/NotificationState.h>
#include <ao/uimodel/status/activity/ActivityStatusModel.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::uimodel::test
{
  TEST_CASE("ActivityStatusModel projects compact state from runtime priority", "[uimodel][unit][status][activity]")
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
} // namespace ao::uimodel::test
