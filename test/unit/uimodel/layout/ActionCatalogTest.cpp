// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/ActionCatalog.h>
#include <ao/uimodel/layout/ActionTypes.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::uimodel::layout::test
{
  TEST_CASE("ActionCatalog duplicate registration preserves the original descriptor",
            "[uimodel][unit][layout][catalog]")
  {
    auto catalog = ActionCatalog{};

    SECTION("register and retrieve descriptor")
    {
      auto const registered =
        catalog.registerActionDescriptor(ActionDescriptor{.id = "playback.playPause",
                                                          .label = "Play/Pause",
                                                          .category = "Playback",
                                                          .capabilities = ActionCapability::None});
      CHECK(registered == true);

      auto const optDesc = catalog.descriptor("playback.playPause");
      REQUIRE(optDesc.has_value());
      CHECK(optDesc->id == "playback.playPause");
      CHECK(optDesc->label == "Play/Pause");
      CHECK(optDesc->category == "Playback");
    }

    SECTION("rejects duplicate id")
    {
      CHECK(catalog.registerActionDescriptor(ActionDescriptor{
        .id = "test", .label = "A", .category = "X", .capabilities = ActionCapability::RequiresAnchor}));
      CHECK(catalog.registerActionDescriptor(ActionDescriptor{
              .id = "test", .label = "B", .category = "Y", .capabilities = ActionCapability::RequiresFocusedView}) ==
            false);

      auto const optDesc = catalog.descriptor("test");
      REQUIRE(optDesc.has_value());
      CHECK(optDesc->id == "test");
      CHECK(optDesc->label == "A");
      CHECK(optDesc->category == "X");
      CHECK(optDesc->capabilities.has(ActionCapability::RequiresAnchor));
      CHECK(optDesc->capabilities.has(ActionCapability::RequiresFocusedView) == false);

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
      catalog.registerActionDescriptor(ActionDescriptor{.id = "first", .label = "First", .category = "A"});
      catalog.registerActionDescriptor(ActionDescriptor{.id = "second", .label = "Second", .category = "B"});

      auto const all = catalog.descriptors();
      REQUIRE(all.size() == 2);
      CHECK(all[0].id == "first");
      CHECK(all[1].id == "second");
    }
  }

  TEST_CASE("ActionCatalog canBind rejects actions missing required context", "[uimodel][unit][layout][catalog]")
  {
    auto catalog = ActionCatalog{};

    catalog.registerActionDescriptor(ActionDescriptor{.id = "needsAnchor",
                                                      .label = "Needs Anchor",
                                                      .category = "Test",
                                                      .capabilities = ActionCapability::RequiresAnchor});

    catalog.registerActionDescriptor(ActionDescriptor{.id = "needsFocusedView",
                                                      .label = "Needs Focus",
                                                      .category = "Test",
                                                      .capabilities = ActionCapability::RequiresFocusedView});

    catalog.registerActionDescriptor(ActionDescriptor{
      .id = "noRequirements", .label = "Simple", .category = "Test", .capabilities = ActionCapability::None});

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
      auto ctx = ActionBindingContext{};
      ctx.hasAnchor = false;
      CHECK(catalog.canBind("needsAnchor", ctx) == false);
    }

    SECTION("allows anchor-requiring action with anchor")
    {
      auto ctx = ActionBindingContext{};
      ctx.hasAnchor = true;
      CHECK(catalog.canBind("needsAnchor", ctx) == true);
    }

    SECTION("rejects focused-view action without focused view")
    {
      auto ctx = ActionBindingContext{};
      ctx.hasFocusedView = false;
      CHECK(catalog.canBind("needsFocusedView", ctx) == false);
    }

    SECTION("allows focused-view action with focused view")
    {
      auto ctx = ActionBindingContext{};
      ctx.hasFocusedView = true;
      CHECK(catalog.canBind("needsFocusedView", ctx) == true);
    }
  }

  TEST_CASE("ActionCatalog tryBind returns a bound action when context is valid", "[uimodel][unit][layout][catalog]")
  {
    auto catalog = ActionCatalog{};

    catalog.registerActionDescriptor(
      ActionDescriptor{.id = "valid", .label = "Valid", .category = "Test", .capabilities = ActionCapability::None});

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
} // namespace ao::uimodel::layout::test
