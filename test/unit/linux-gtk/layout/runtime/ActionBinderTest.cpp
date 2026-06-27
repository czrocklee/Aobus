// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/ActionBinder.h"

#include "layout/runtime/ActionRegistry.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/uimodel/layout/ActionTypes.h>
#include <ao/uimodel/layout/LayoutNode.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/box.h>
#include <gtkmm/window.h>

#include <string>

namespace ao::gtk::layout::test
{
  using namespace uimodel::layout;
  using namespace ao::gtk::test;

  TEST_CASE("ActionBinder binds layout action properties to activation callbacks", "[gtk][unit][layout][runtime]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto window = Gtk::Window{};
    auto anchor = Gtk::Box{};

    auto registry = ActionRegistry{};

    auto lastFiredId = std::string{};
    auto lastComponentId = std::string{};
    Gtk::Widget* lastAnchor = nullptr;

    registry.registerAction(ActionDescriptor{.id = "test.action",
                                             .label = "Test Action",
                                             .category = "Test",
                                             .capabilities = ActionCapability::RequiresAnchor},
                            [&](ActionActivationContext& ctx)
                            {
                              lastFiredId = "test.action";
                              lastComponentId = ctx.componentId;
                              lastAnchor = &ctx.anchorWidget;
                            });

    // Binder doesn't need LayoutContext, only registry, runtime, and parent window
    auto const binder = ActionBinder{registry, runtime, window};

    SECTION("bind returns empty function for 'none'")
    {
      auto const node = LayoutNode{.type = "test.node"};
      auto const cb = binder.bind(node, "action", "none", ActionSlot::PrimaryClick, anchor);
      CHECK_FALSE(cb);
    }

    SECTION("bind returns empty function for unknown action")
    {
      auto node = LayoutNode{.type = "test.node"};
      node.props["action"] = LayoutValue{std::string{"unknown.action"}};
      auto const cb = binder.bind(node, "action", "none", ActionSlot::PrimaryClick, anchor);
      CHECK_FALSE(cb);
    }

    SECTION("bind returns valid function and passes correct context")
    {
      auto node = LayoutNode{.id = "my-component", .type = "test.node"};
      node.props["action"] = LayoutValue{std::string{"test.action"}};

      auto const cb = binder.bind(node, "action", "none", ActionSlot::PrimaryClick, anchor);
      REQUIRE(cb);

      cb();

      CHECK(lastFiredId == "test.action");
      CHECK(lastComponentId == "my-component");
      CHECK(lastAnchor == &anchor);
    }

    SECTION("bind uses default action ID if property is missing")
    {
      auto const node = LayoutNode{.id = "default-comp", .type = "test.node"};

      auto const cb = binder.bind(node, "missing", "test.action", ActionSlot::PrimaryClick, anchor);
      REQUIRE(cb);

      cb();

      CHECK(lastFiredId == "test.action");
      CHECK(lastComponentId == "default-comp");
    }
  }
} // namespace ao::gtk::layout::test
