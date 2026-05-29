// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/ComponentCatalog.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::uimodel::layout::test
{
  TEST_CASE("ComponentCatalog descriptor registration", "[layout][unit][catalog]")
  {
    auto catalog = ComponentCatalog{};

    SECTION("register and retrieve descriptor")
    {
      auto const registered = catalog.registerComponentDescriptor(
        ComponentDescriptor{.type = "playback.playPauseButton",
                            .displayName = "Play/Pause Button",
                            .category = "Playback",
                            .props = {{.name = "showLabel", .kind = PropertyKind::Bool, .label = "Show Label"}}});
      CHECK(registered == true);

      auto const optDesc = catalog.descriptor("playback.playPauseButton");
      REQUIRE(optDesc.has_value());
      CHECK(optDesc->type == "playback.playPauseButton");
      CHECK(optDesc->displayName == "Play/Pause Button");
      CHECK(optDesc->category == "Playback");
      CHECK(optDesc->props.size() == 1);
      CHECK(optDesc->props[0].name == "showLabel");
    }

    SECTION("rejects duplicate type")
    {
      CHECK(catalog.registerComponentDescriptor(
        ComponentDescriptor{.type = "box", .displayName = "Box", .category = "Containers"}));
      CHECK(catalog.registerComponentDescriptor(
              ComponentDescriptor{.type = "box", .displayName = "Box Duplicate", .category = "Containers"}) == false);
    }

    SECTION("returns nullopt for unknown type")
    {
      auto const optDesc = catalog.descriptor("nonexistent");
      CHECK(optDesc.has_value() == false);
    }

    SECTION("returns descriptors in registration order")
    {
      catalog.registerComponentDescriptor(ComponentDescriptor{.type = "a", .displayName = "A", .category = "X"});
      catalog.registerComponentDescriptor(ComponentDescriptor{.type = "b", .displayName = "B", .category = "Y"});

      auto const& all = catalog.descriptors();
      REQUIRE(all.size() == 2);
      CHECK(all[0].type == "a");
      CHECK(all[1].type == "b");
    }
  }
}
