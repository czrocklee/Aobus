// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/uimodel/status/ActivityStatusModelTestSupport.h"
#include <ao/rt/StateTypes.h>
#include <ao/uimodel/status/ActivityStatusModel.h>

#include <catch2/catch_test_macros.hpp>

#include <optional>

namespace ao::uimodel::status::test
{
  TEST_CASE("ActivityStatusModel applies notification activity presentation policy",
            "[uimodel][unit][status][activity]")
  {
    auto model = ActivityStatusModel{};

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
  }
} // namespace ao::uimodel::status::test
