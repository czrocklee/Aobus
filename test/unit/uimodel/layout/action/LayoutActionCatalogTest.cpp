// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/action/LayoutActionCapabilities.h>
#include <ao/uimodel/layout/action/LayoutActionCatalog.h>
#include <ao/uimodel/layout/action/LayoutActionDescriptor.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::uimodel::test
{
  TEST_CASE("LayoutActionCatalog - duplicate registration preserves the original descriptor",
            "[uimodel][unit][layout][action]")
  {
    auto catalog = LayoutActionCatalog{};

    SECTION("register and retrieve descriptor")
    {
      auto const registered =
        catalog.registerActionDescriptor(LayoutActionDescriptor{.id = "playback.playPause",
                                                                .label = "Play/Pause",
                                                                .category = "Playback",
                                                                .capabilities = LayoutActionCapability::None});
      CHECK(registered == true);

      auto const optDesc = catalog.descriptor("playback.playPause");
      REQUIRE(optDesc);
      CHECK(optDesc->id == "playback.playPause");
      CHECK(optDesc->label == "Play/Pause");
      CHECK(optDesc->category == "Playback");
    }

    SECTION("rejects duplicate id")
    {
      CHECK(catalog.registerActionDescriptor(LayoutActionDescriptor{
        .id = "test", .label = "A", .category = "X", .capabilities = LayoutActionCapability::RequiresAnchor}));
      CHECK(catalog.registerActionDescriptor(LayoutActionDescriptor{
              .id = "test", .label = "B", .category = "Y", .capabilities = LayoutActionCapability::PresentsMenu}) ==
            false);

      auto const optDesc = catalog.descriptor("test");
      REQUIRE(optDesc);
      CHECK(optDesc->id == "test");
      CHECK(optDesc->label == "A");
      CHECK(optDesc->category == "X");
      CHECK(optDesc->capabilities.has(LayoutActionCapability::RequiresAnchor));
      CHECK_FALSE(optDesc->capabilities.has(LayoutActionCapability::PresentsMenu));

      auto const all = catalog.descriptors();
      REQUIRE(all.size() == 1);
      CHECK(all.front().label == "A");
    }

    SECTION("returns nullopt for unknown id")
    {
      auto const optDesc = catalog.descriptor("nonexistent");
      CHECK_FALSE(optDesc);
    }

    SECTION("returns all descriptors in registration order")
    {
      catalog.registerActionDescriptor(LayoutActionDescriptor{.id = "first", .label = "First", .category = "A"});
      catalog.registerActionDescriptor(LayoutActionDescriptor{.id = "second", .label = "Second", .category = "B"});

      auto const all = catalog.descriptors();
      REQUIRE(all.size() == 2);
      CHECK(all[0].id == "first");
      CHECK(all[1].id == "second");
    }
  }
} // namespace ao::uimodel::test
