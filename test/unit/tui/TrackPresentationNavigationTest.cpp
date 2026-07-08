// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/TrackPresentationNavigation.h"

#include <ao/rt/TrackPresentation.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace ao::tui::test
{
  TEST_CASE("TrackPresentationNavigation - includes builtin and custom views", "[tui][unit][track-presentation]")
  {
    auto const custom = std::vector<rt::CustomTrackPresentationPreset>{
      {.label = "Dense Albums", .basePresetId = "albums", .spec = rt::TrackPresentationSpec{.id = "dense-albums"}},
    };

    auto const items = makeTrackPresentationNavigation(rt::builtinTrackPresentationPresets(), custom);

    REQUIRE(items.size() > custom.size());
    CHECK(items[0].id == "library");
    CHECK(items[0].label == "Library");
    CHECK(items[0].detail == "All tracks in album-artist and album order.");
    CHECK(items.back().id == "dense-albums");
    CHECK(items.back().label == "Dense Albums");
    CHECK(items.back().detail == "custom from albums");
  }

  TEST_CASE("TrackPresentationNavigation - falls back for sparse preset labels", "[tui][unit][track-presentation]")
  {
    auto const builtin = std::vector<rt::TrackPresentationPreset>{
      {.spec = rt::TrackPresentationSpec{.id = "raw"}},
    };
    auto const custom = std::vector<rt::CustomTrackPresentationPreset>{
      {.spec = rt::TrackPresentationSpec{.id = "custom-raw"}},
    };

    auto const items = makeTrackPresentationNavigation(builtin, custom);

    REQUIRE(items.size() == 2);
    CHECK(items[0].label == "raw");
    CHECK(items[1].label == "custom-raw");
    CHECK(items[1].detail == "custom");
  }

  TEST_CASE("TrackPresentationNavigation - display labels fall back to default", "[tui][unit][track-presentation]")
  {
    CHECK(trackPresentationDisplayId("") == "default");
    CHECK(trackPresentationDisplayId("albums") == "albums");
    CHECK(trackPresentationBadgeLabel("") == "view:default");
    CHECK(trackPresentationBadgeLabel("albums") == "view:albums");
  }
} // namespace ao::tui::test
