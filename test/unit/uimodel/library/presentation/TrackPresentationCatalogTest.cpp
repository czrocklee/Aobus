// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/uimodel/library/presentation/TrackPresentationTestSupport.h"
#include <ao/rt/TrackPresentation.h>
#include <ao/uimodel/library/presentation/TrackPresentationCatalog.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace ao::uimodel::test
{
  TEST_CASE("TrackPresentationCatalog - projects builtin and custom presentation choices",
            "[uimodel][unit][library][presentation]")
  {
    auto fixture = TrackPresentationFixture{};
    auto& catalog = fixture.catalog;
    std::int32_t signalCount = 0;
    auto sub = catalog.signalChanged().connect([&signalCount] { ++signalCount; });

    catalog.addCustomPresentation(rt::CustomTrackPresentationPreset{
      .label = "Library QA",
      .basePresetId = std::string{rt::kDefaultTrackPresentationId},
      .spec = rt::TrackPresentationSpec{.id = "library-qa"},
    });

    auto const items = catalog.menuItems();
    auto const builtinCount = catalog.builtinPresets().size();

    CHECK(catalog.labelForId(rt::kDefaultTrackPresentationId) == "Library");
    CHECK(catalog.labelForId("library-qa") == "Library QA");
    CHECK(catalog.labelForId("missing-preset") == "missing-preset");
    REQUIRE(catalog.specForId("library-qa").has_value());
    CHECK(catalog.specForId("library-qa")->id == "library-qa");
    CHECK_FALSE(catalog.specForId("missing-preset").has_value());

    REQUIRE(items.size() == builtinCount + 4);
    CHECK(items.front().type == TrackPresentationMenuItemType::Preset);
    CHECK(items[builtinCount].type == TrackPresentationMenuItemType::Separator);
    CHECK(items[builtinCount + 1].type == TrackPresentationMenuItemType::Preset);
    CHECK(items[builtinCount + 1].id == "library-qa");
    CHECK(items[builtinCount + 1].label == "Library QA");
    CHECK(items.back().type == TrackPresentationMenuItemType::CreateCustomView);
    CHECK(signalCount == 1);

    catalog.removeCustomPresentation("library-qa");

    CHECK_FALSE(catalog.specForId("library-qa").has_value());
    CHECK(signalCount == 2);
  }

  TEST_CASE("TrackPresentationCatalog - omits custom separator when no custom presets exist",
            "[uimodel][unit][library][presentation]")
  {
    auto fixture = TrackPresentationFixture{};
    auto& catalog = fixture.catalog;

    auto const items = catalog.menuItems();
    auto const builtinCount = catalog.builtinPresets().size();
    auto const separatorCount =
      std::ranges::count(items, TrackPresentationMenuItemType::Separator, &TrackPresentationMenuItem::type);

    REQUIRE(items.size() == builtinCount + 2);
    CHECK(separatorCount == 1);
    CHECK(items[builtinCount].type == TrackPresentationMenuItemType::Separator);
    CHECK(items.back().type == TrackPresentationMenuItemType::CreateCustomView);
  }
} // namespace ao::uimodel::test
