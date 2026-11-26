// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/TrackPresentationNavigation.h"

#include "test/MessageCatalogTestSupport.h"
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/VirtualListIds.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace ao::tui::test
{
  TEST_CASE("TrackPresentationNavigation - includes builtin and custom views")
  {
    auto const custom = std::vector<rt::CustomTrackPresentationPreset>{
      {.label = "Dense Albums",
       .optBasePresetId = rt::BuiltinPreset::Albums,
       .spec = {.presetId = rt::CustomPresetId{1}}},
    };

    auto const items = makeTrackPresentationNavigation(
      ao::test::englishMessageCatalog(), rt::kAllTracksListId, rt::builtinTrackPresentationPresets(), custom);

    REQUIRE(items.size() > custom.size());
    CHECK(items[0].id == "library");
    CHECK(items[0].label == "Library");
    CHECK(items[0].detail == "All tracks in album-artist and album order.");
    CHECK(items.back().id == "0000000000000001");
    CHECK(items.back().label == "Dense Albums");
    CHECK(items.back().detail == "custom from albums");
  }

  TEST_CASE("TrackPresentationNavigation - an unlabeled custom preset displays its canonical key")
  {
    auto const custom = std::vector<rt::CustomTrackPresentationPreset>{
      {.spec = {.presetId = rt::CustomPresetId{0xabc}}},
    };

    auto const items =
      makeTrackPresentationNavigation(ao::test::englishMessageCatalog(), rt::kAllTracksListId, {}, custom);

    REQUIRE(items.size() == 1);
    CHECK(items.front().label == "0000000000000abc");
    CHECK(items.front().detail == "custom");
  }

  TEST_CASE("TrackPresentationNavigation - preserves complete builtin then custom order")
  {
    auto const builtin = std::vector{
      rt::builtinTrackPresentationPreset(rt::BuiltinPreset::Songs),
      rt::builtinTrackPresentationPreset(rt::BuiltinPreset::Albums),
    };
    auto const custom = std::vector<rt::CustomTrackPresentationPreset>{
      {.label = "Dense Albums",
       .optBasePresetId = rt::BuiltinPreset::Albums,
       .spec = {.presetId = rt::CustomPresetId{2}}},
      {.label = "Simple", .spec = {.presetId = rt::CustomPresetId{1}}},
    };

    auto const items =
      makeTrackPresentationNavigation(ao::test::englishMessageCatalog(), rt::kAllTracksListId, builtin, custom);

    REQUIRE(items.size() == 4);
    CHECK(items[0].id == "songs");
    CHECK(items[0].label == "Songs");
    CHECK(items[1].id == "albums");
    CHECK(items[1].label == "Albums");
    CHECK(items[2].id == "0000000000000002");
    CHECK(items[2].label == "Dense Albums");
    CHECK(items[2].detail == "custom from albums");
    CHECK(items[3].id == "0000000000000001");
    CHECK(items[3].label == "Simple");
    CHECK(items[3].detail == "custom");
  }

  TEST_CASE("TrackPresentationNavigation - renders locale-selected navigation copy")
  {
    auto const textCatalog = ao::test::messageCatalog("de-DE");
    auto const custom = std::vector<rt::CustomTrackPresentationPreset>{
      {.label = "Dicht", .optBasePresetId = rt::BuiltinPreset::Albums, .spec = {.presetId = rt::CustomPresetId{1}}},
    };

    auto const items = makeTrackPresentationNavigation(textCatalog, rt::kAllTracksListId, {}, custom);

    REQUIRE(items.size() == 1);
    CHECK(items.front().id == "0000000000000001");
    CHECK(items.front().label == "Dicht");
    CHECK(items.front().detail == "benutzerdefiniert aus albums");
    CHECK(trackPresentationDisplayLabel(textCatalog, rt::CustomPresetId{1}, items) == "Dicht");
    CHECK(trackPresentationDisplayLabel(textCatalog, rt::CustomPresetId{2}, items) == "0000000000000002");
    CHECK(trackPresentationDisplayLabel(textCatalog, rt::BuiltinPreset::Albums, items) == "Alben");
  }
} // namespace ao::tui::test
