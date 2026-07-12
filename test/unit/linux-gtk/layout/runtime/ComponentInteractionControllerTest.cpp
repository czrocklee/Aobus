// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/ComponentInteractionController.h"

#include "app/linux-gtk/app/GtkUiDependencies.h"
#include "layout/runtime/ActionRegistry.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutRuntimeState.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/uimodel/layout/action/LayoutActionSlot.h>
#include <ao/uimodel/layout/component/LayoutComponentActionPolicy.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/gesturelongpress.h>
#include <gtkmm/window.h>

namespace ao::gtk::layout::test
{
  using namespace uimodel;
  using namespace ao::gtk::test;

  TEST_CASE("ComponentInteractionController - routes configured gestures to layout actions",
            "[gtk][unit][layout][runtime]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto window = Gtk::Window{};
    auto registry = ActionRegistry{};
    auto compRegistry = ComponentRegistry{};

    bool primaryClicked = false;
    bool secondaryClicked = false;
    bool primaryLongPressed = false;
    bool secondaryLongPressed = false;

    registry.registerAction(
      {.id = "primary", .label = "Primary", .category = "Test"}, [&](auto&) { primaryClicked = true; });
    registry.registerAction(
      {.id = "secondary", .label = "Secondary", .category = "Test"}, [&](auto&) { secondaryClicked = true; });
    registry.registerAction(
      {.id = "primaryLong", .label = "Primary Long", .category = "Test"}, [&](auto&) { primaryLongPressed = true; });
    registry.registerAction({.id = "secondaryLong", .label = "Secondary Long", .category = "Test"},
                            [&](auto&) { secondaryLongPressed = true; });

    auto runtimeState = LayoutRuntimeState{};
    auto dependencies = GtkUiDependencies{};
    auto ctx = LayoutBuildContext{.registry = compRegistry,
                                  .actionRegistry = registry,
                                  .runtime = fixture.runtime(),
                                  .parentWindow = window,
                                  .runtimeState = runtimeState,
                                  .dependencies = dependencies};

    SECTION("attaches primary click to Gtk::Button")
    {
      auto button = Gtk::Button{};
      auto node = uimodel::LayoutNode{.type = "btn"};
      node.props[std::string{uimodel::kPrimaryActionProp}] = uimodel::LayoutValue{std::string{"primary"}};

      auto controller = ComponentInteractionController{};
      controller.attach(ctx, node, button, uimodel::kAllExternalActions);

      emitClicked(button);
      CHECK(primaryClicked);
    }

    SECTION("attaches and triggers default actions when node props are missing")
    {
      auto button = Gtk::Button{};
      auto node = uimodel::LayoutNode{.type = "btn"}; // No props

      auto policy = uimodel::kExternalPrimaryActions;
      policy.defaultActionIds.emplace_back(LayoutActionSlot::PrimaryClick, "primary");

      auto controller = ComponentInteractionController{};
      controller.attach(ctx, node, button, policy);

      emitClicked(button);
      CHECK(primaryClicked);
    }

    SECTION("secondary and long-press gestures dispatch configured actions")
    {
      auto box = Gtk::Box{};
      auto node = uimodel::LayoutNode{.type = "box"};
      node.props[std::string{uimodel::kSecondaryActionProp}] = uimodel::LayoutValue{std::string{"secondary"}};
      node.props[std::string{uimodel::kPrimaryLongPressActionProp}] = uimodel::LayoutValue{std::string{"primaryLong"}};

      auto controller = ComponentInteractionController{};
      controller.attach(ctx, node, box, uimodel::kAllExternalActions);

      REQUIRE(emitGestureReleased(box));
      CHECK(secondaryClicked);
      CHECK_FALSE(primaryLongPressed);

      auto const longPressPtr = findController<Gtk::GestureLongPress>(box);
      REQUIRE(longPressPtr);
      ::g_signal_emit_by_name(longPressPtr->gobj(), "pressed", 1.0, 1.0);
      CHECK(primaryLongPressed);
    }

    SECTION("respects policy and ignores disallowed slots")
    {
      auto button = Gtk::Button{};
      auto node = uimodel::LayoutNode{.type = "btn"};
      node.props[std::string{uimodel::kPrimaryActionProp}] = uimodel::LayoutValue{std::string{"primary"}};
      node.props[std::string{uimodel::kSecondaryActionProp}] = uimodel::LayoutValue{std::string{"secondary"}};

      auto controller = ComponentInteractionController{};
      // Only allow secondary
      controller.attach(ctx, node, button, uimodel::kExternalSecondaryActions);

      emitClicked(button);
      CHECK_FALSE(primaryClicked);
    }
  }
} // namespace ao::gtk::layout::test
