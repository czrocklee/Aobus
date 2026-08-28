// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/ActionBinder.h"

#include "layout/runtime/ActionRegistry.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include <ao/uimodel/layout/component/LayoutSchema.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/box.h>
#include <gtkmm/window.h>

#include <string>

namespace ao::gtk::layout::test
{
  using namespace uimodel;
  using namespace ao::gtk::test;

  TEST_CASE("ActionBinder - binds layout action properties to activation callbacks", "[gtk][unit][layout][runtime]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto window = Gtk::Window{};
    auto anchor = Gtk::Box{};

    auto layoutSchema = LayoutSchema{};
    auto registry = ActionRegistry{layoutSchema};

    auto lastFiredId = std::string{};
    auto lastComponentId = std::string{};
    Gtk::Widget* lastAnchor = nullptr;

    registry.registerAction(ActionSchema{.id = "test.action",
                                         .label = "Test Action",
                                         .category = "Test",
                                         .capabilities = actionCapabilityBit(ActionCapability::RequiresAnchor)},
                            [&](ActionActivationContext& ctx)
                            {
                              lastFiredId = "test.action";
                              lastComponentId = ctx.componentId;
                              lastAnchor = &ctx.anchorWidget;
                            });

    // Binder doesn't need LayoutBuildContext, only registry and parent window
    auto const binder = ActionBinder{registry, window};
    auto const componentSchema = ComponentSchema{
      .id = "test.node",
      .displayName = "Test Node",
      .actionSlots = actionSlotBit(ActionSlot::PrimaryClick),
      .defaultActions = {{.slot = ActionSlot::PrimaryClick, .actionId = "none"}},
    };

    SECTION("bind returns empty function for 'none'")
    {
      auto const node = LayoutNode{.type = "test.node"};
      auto const cb = binder.bind(node, componentSchema, ActionSlot::PrimaryClick, anchor);
      CHECK_FALSE(cb);
    }

    SECTION("bind returns empty function for unknown action")
    {
      auto node = LayoutNode{.type = "test.node"};
      node.props[std::string{kPrimaryActionProp}] = LayoutValue{std::string{"unknown.action"}};
      auto const cb = binder.bind(node, componentSchema, ActionSlot::PrimaryClick, anchor);
      CHECK_FALSE(cb);
    }

    SECTION("bind returns valid function and passes correct context")
    {
      auto node = LayoutNode{.id = "my-component", .type = "test.node"};
      node.props[std::string{kPrimaryActionProp}] = LayoutValue{std::string{"test.action"}};

      auto const cb = binder.bind(node, componentSchema, ActionSlot::PrimaryClick, anchor);
      REQUIRE(cb);

      cb();

      CHECK(lastFiredId == "test.action");
      CHECK(lastComponentId == "my-component");
      CHECK(lastAnchor == &anchor);
    }

    SECTION("bind uses default action ID if property is missing")
    {
      auto const node = LayoutNode{.id = "default-comp", .type = "test.node"};

      auto defaultSchema = componentSchema;
      defaultSchema.defaultActions[0].actionId = "test.action";
      auto const cb = binder.bind(node, defaultSchema, ActionSlot::PrimaryClick, anchor);
      REQUIRE(cb);

      cb();

      CHECK(lastFiredId == "test.action");
      CHECK(lastComponentId == "default-comp");
    }
  }
} // namespace ao::gtk::layout::test
