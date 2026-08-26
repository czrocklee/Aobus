// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/winui/track/TrackRevealAdapter.h>

#include <ao/CoreIds.h>
#include <ao/rt/ViewIds.h>
#include <ao/uimodel/library/track/TrackDisplayIndex.h>

#include <catch2/catch_test_macros.hpp>

#include <optional>

namespace ao::winui::test
{
  TEST_CASE("Track reveal adapter - retains intent until source rows become available",
            "[winui][regression][track-reveal]")
  {
    auto intent = TrackRevealIntent{};
    auto displayIndex = uimodel::TrackDisplayIndex{};
    auto const viewId = rt::ViewId{7};

    recordTrackRevealIntent(intent, viewId, TrackId{42});

    CHECK_FALSE(resolveTrackRevealTarget(intent, viewId, std::nullopt, displayIndex));

    REQUIRE(displayIndex.reset(3, {}));
    CHECK(resolveTrackRevealTarget(intent, viewId, 1, displayIndex) == TrackRevealTarget{
                                                                         .serial = 1,
                                                                         .displayIndex = 1,
                                                                       });
  }

  TEST_CASE("Track reveal adapter - rejects stale views and advances request identity", "[winui][unit][track-reveal]")
  {
    auto intent = TrackRevealIntent{};
    auto displayIndex = uimodel::TrackDisplayIndex{};
    REQUIRE(displayIndex.reset(1, {}));

    recordTrackRevealIntent(intent, rt::ViewId{3}, TrackId{8});
    recordTrackRevealIntent(intent, rt::ViewId{4}, TrackId{9});

    CHECK_FALSE(resolveTrackRevealTarget(intent, rt::ViewId{3}, 0, displayIndex));
    CHECK(resolveTrackRevealTarget(intent, rt::ViewId{4}, 0, displayIndex) == TrackRevealTarget{
                                                                                .serial = 2,
                                                                                .displayIndex = 0,
                                                                              });
  }
} // namespace ao::winui::test
