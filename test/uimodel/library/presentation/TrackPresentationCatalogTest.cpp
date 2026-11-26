// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/presentation/TrackPresentationCatalog.h>

#include "test/MessageCatalogTestSupport.h"
#include "test/uimodel/library/presentation/TrackPresentationTestSupport.h"
#include <ao/rt/TrackPresentation.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace ao::uimodel::test
{
  TEST_CASE("TrackPresentationCatalog - language replacement notifies observers after labels change")
  {
    auto fixture = TrackPresentationFixture{};
    auto& catalog = fixture.catalog;
    auto const optPreviousSpec = catalog.specForId(rt::BuiltinPreset::Library);
    std::int32_t notifications = 0;
    auto observedLabel = std::string{};
    auto sub = catalog.signalChanged().connect(
      [&]
      {
        ++notifications;
        observedLabel = catalog.labelFor(rt::BuiltinPreset::Library);
      });
    catalog.setTextCatalog(ao::test::messageCatalog("de"));
    CHECK(notifications == 1);
    CHECK(observedLabel == "Bibliothek");
    CHECK(catalog.specForId(rt::BuiltinPreset::Library) == optPreviousSpec);
  }

  TEST_CASE("TrackPresentationCatalog - projects builtin and custom presentation choices")
  {
    auto fixture = TrackPresentationFixture{};
    auto& catalog = fixture.catalog;
    std::int32_t signalCount = 0;
    auto sub = catalog.signalChanged().connect([&signalCount] noexcept { ++signalCount; });

    auto const customId = rt::CustomPresetId{0x1a};
    catalog.addCustomPresentation(rt::CustomTrackPresentationPreset{
      .label = "Library QA",
      .optBasePresetId = rt::TrackPresetId{rt::BuiltinPreset::Library},
      .spec = rt::TrackPresentationSpec{.presetId = customId},
    });

    auto const items = catalog.menuItems();
    auto const builtinCount = catalog.builtinPresets().size();

    CHECK(catalog.labelFor(rt::BuiltinPreset::Library) == "Library");
    CHECK(catalog.labelFor(rt::BuiltinPreset::ListOrder) == "Manual Order");
    CHECK(catalog.labelFor(customId) == "Library QA");
    auto const optSpec = catalog.specForId(customId);
    REQUIRE(optSpec);
    CHECK(optSpec->isPreset(customId));
    auto const optListOrder = catalog.specForId(rt::BuiltinPreset::ListOrder);
    REQUIRE(optListOrder);
    CHECK(optListOrder->sortBy.empty());
    CHECK_FALSE(catalog.specForId(rt::CustomPresetId{0x2b}));

    REQUIRE(items.size() == builtinCount + 4);
    CHECK(items.front().type == TrackPresentationMenuItemType::Preset);
    CHECK(items[builtinCount].type == TrackPresentationMenuItemType::Separator);
    CHECK(items[builtinCount + 1].type == TrackPresentationMenuItemType::Preset);
    CHECK(items[builtinCount + 1].id == "000000000000001a");
    CHECK(items[builtinCount + 1].label == "Library QA");
    CHECK(items.back().type == TrackPresentationMenuItemType::CreateCustomView);
    CHECK(signalCount == 1);
  }

  TEST_CASE("TrackPresentationCatalog - omits custom separator when no custom presets exist")
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
