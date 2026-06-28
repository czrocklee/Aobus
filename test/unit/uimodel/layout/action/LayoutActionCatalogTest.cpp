// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/action/LayoutActionCatalog.h>
#include <ao/uimodel/layout/action/LayoutActionTypes.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::uimodel::test
{
  TEST_CASE("LayoutActionCatalog duplicate registration preserves the original descriptor",
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
      REQUIRE(optDesc.has_value());
      CHECK(optDesc->id == "playback.playPause");
      CHECK(optDesc->label == "Play/Pause");
      CHECK(optDesc->category == "Playback");
    }

    SECTION("rejects duplicate id")
    {
      CHECK(catalog.registerActionDescriptor(LayoutActionDescriptor{
        .id = "test", .label = "A", .category = "X", .capabilities = LayoutActionCapability::RequiresAnchor}));
      CHECK(
        catalog.registerActionDescriptor(LayoutActionDescriptor{
          .id = "test", .label = "B", .category = "Y", .capabilities = LayoutActionCapability::RequiresFocusedView}) ==
        false);

      auto const optDesc = catalog.descriptor("test");
      REQUIRE(optDesc.has_value());
      CHECK(optDesc->id == "test");
      CHECK(optDesc->label == "A");
      CHECK(optDesc->category == "X");
      CHECK(optDesc->capabilities.has(LayoutActionCapability::RequiresAnchor));
      CHECK(optDesc->capabilities.has(LayoutActionCapability::RequiresFocusedView) == false);

      auto const all = catalog.descriptors();
      REQUIRE(all.size() == 1);
      CHECK(all.front().label == "A");
    }

    SECTION("returns nullopt for unknown id")
    {
      auto const optDesc = catalog.descriptor("nonexistent");
      CHECK(optDesc.has_value() == false);
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

  TEST_CASE("LayoutActionCatalog canBind rejects actions missing required context", "[uimodel][unit][layout][action]")
  {
    auto catalog = LayoutActionCatalog{};

    catalog.registerActionDescriptor(LayoutActionDescriptor{.id = "needsAnchor",
                                                            .label = "Needs Anchor",
                                                            .category = "Test",
                                                            .capabilities = LayoutActionCapability::RequiresAnchor});

    catalog.registerActionDescriptor(
      LayoutActionDescriptor{.id = "needsFocusedView",
                             .label = "Needs Focus",
                             .category = "Test",
                             .capabilities = LayoutActionCapability::RequiresFocusedView});

    catalog.registerActionDescriptor(LayoutActionDescriptor{
      .id = "noRequirements", .label = "Simple", .category = "Test", .capabilities = LayoutActionCapability::None});

    SECTION("returns false for unknown action id")
    {
      CHECK(catalog.canBind("unknown", {}) == false);
    }

    SECTION("allows actions with no requirements in any context")
    {
      CHECK(catalog.canBind("noRequirements", {}) == true);
    }

    SECTION("rejects anchor-requiring action without anchor")
    {
      auto ctx = LayoutActionBindingContext{};
      ctx.hasAnchor = false;
      CHECK(catalog.canBind("needsAnchor", ctx) == false);
    }

    SECTION("allows anchor-requiring action with anchor")
    {
      auto ctx = LayoutActionBindingContext{};
      ctx.hasAnchor = true;
      CHECK(catalog.canBind("needsAnchor", ctx) == true);
    }

    SECTION("rejects focused-view action without focused view")
    {
      auto ctx = LayoutActionBindingContext{};
      ctx.hasFocusedView = false;
      CHECK(catalog.canBind("needsFocusedView", ctx) == false);
    }

    SECTION("allows focused-view action with focused view")
    {
      auto ctx = LayoutActionBindingContext{};
      ctx.hasFocusedView = true;
      CHECK(catalog.canBind("needsFocusedView", ctx) == true);
    }
  }

  TEST_CASE("LayoutActionCatalog tryBind returns a bound action when context is valid",
            "[uimodel][unit][layout][action]")
  {
    auto catalog = LayoutActionCatalog{};

    catalog.registerActionDescriptor(LayoutActionDescriptor{
      .id = "valid", .label = "Valid", .category = "Test", .capabilities = LayoutActionCapability::None});

    SECTION("rejects empty id")
    {
      CHECK(catalog.tryBind("", {}) == false);
    }

    SECTION("rejects none id")
    {
      CHECK(catalog.tryBind("none", {}) == false);
    }

    SECTION("returns true for valid bindable action")
    {
      CHECK(catalog.tryBind("valid", {}) == true);
    }
  }
} // namespace ao::uimodel::test
