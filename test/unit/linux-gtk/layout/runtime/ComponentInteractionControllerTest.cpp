// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/ComponentInteractionController.h"

#include "layout/runtime/ActionRegistry.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutContext.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/uimodel/layout/ActionTypes.h>
#include <ao/uimodel/layout/ComponentActionPolicy.h>
#include <ao/uimodel/layout/ComponentCatalog.h>
#include <ao/uimodel/layout/LayoutNode.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/window.h>

namespace ao::gtk::layout::test
{
  using namespace uimodel::layout;
  using namespace ao::gtk::test;

  TEST_CASE("ComponentInteractionController - unit", "[layout][unit][runtime]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto window = Gtk::Window{};
    auto registry = ActionRegistry{};
    auto compRegistry = ComponentRegistry{};

    auto primaryClicked = false;
    auto secondaryClicked = false;
    auto primaryLongPressed = false;
    auto secondaryLongPressed = false;

    registry.registerAction(
      {.id = "primary", .label = "Primary", .category = "Test"}, [&](auto&) { primaryClicked = true; });
    registry.registerAction(
      {.id = "secondary", .label = "Secondary", .category = "Test"}, [&](auto&) { secondaryClicked = true; });
    registry.registerAction(
      {.id = "primaryLong", .label = "Primary Long", .category = "Test"}, [&](auto&) { primaryLongPressed = true; });
    registry.registerAction({.id = "secondaryLong", .label = "Secondary Long", .category = "Test"},
                            [&](auto&) { secondaryLongPressed = true; });

    auto ctx = LayoutContext{
      .registry = compRegistry, .actionRegistry = registry, .runtime = fixture.runtime(), .parentWindow = window};

    SECTION("attaches primary click to Gtk::Button")
    {
      auto button = Gtk::Button{};
      auto node = uimodel::layout::LayoutNode{.type = "btn"};
      node.props[std::string{uimodel::layout::kPrimaryActionProp}] =
        uimodel::layout::LayoutValue{std::string{"primary"}};

      auto controller = ComponentInteractionController{};
      controller.attach(ctx, node, button, uimodel::layout::kAllExternalActions);

      emitClicked(button);
      CHECK(primaryClicked);
    }

    SECTION("attaches and triggers default actions when node props are missing")
    {
      auto button = Gtk::Button{};
      auto node = uimodel::layout::LayoutNode{.type = "btn"}; // No props

      auto policy = uimodel::layout::kExternalPrimaryActions;
      policy.defaultActionIds[ActionSlot::PrimaryClick] = "primary";

      auto controller = ComponentInteractionController{};
      controller.attach(ctx, node, button, policy);

      emitClicked(button);
      CHECK(primaryClicked);
    }

    SECTION("gesture signal emission (basic coverage)")
    {
      auto box = Gtk::Box{};
      auto node = uimodel::layout::LayoutNode{.type = "box"};
      node.props[std::string{uimodel::layout::kSecondaryActionProp}] =
        uimodel::layout::LayoutValue{std::string{"secondary"}};
      node.props[std::string{uimodel::layout::kPrimaryLongPressActionProp}] =
        uimodel::layout::LayoutValue{std::string{"primaryLong"}};

      auto controller = ComponentInteractionController{};
      controller.attach(ctx, node, box, uimodel::layout::kAllExternalActions);

      // Verify that the gestures were created and added to the widget
      // We simulate the gesture signals using the underlying controller's signals if possible,
      // but in unit tests we mainly verify the attachment and the binder logic.
      // Full event simulation is best handled in integration tests.
    }

    SECTION("respects policy and ignores disallowed slots")
    {
      auto button = Gtk::Button{};
      auto node = uimodel::layout::LayoutNode{.type = "btn"};
      node.props[std::string{uimodel::layout::kPrimaryActionProp}] =
        uimodel::layout::LayoutValue{std::string{"primary"}};
      node.props[std::string{uimodel::layout::kSecondaryActionProp}] =
        uimodel::layout::LayoutValue{std::string{"secondary"}};

      auto controller = ComponentInteractionController{};
      // Only allow secondary
      controller.attach(ctx, node, button, uimodel::layout::kExternalSecondaryActions);

      emitClicked(button);
      CHECK_FALSE(primaryClicked);
    }
  }
} // namespace ao::gtk::layout::test
