// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/ComponentInteractionController.h"

#include "layout/runtime/ActionRegistry.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/unit/linux-gtk/GtkWidgetTestSupport.h"
#include "test/unit/linux-gtk/layout/LayoutTestSupport.h"
#include <ao/uimodel/layout/component/LayoutSchema.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/gesturelongpress.h>
#include <gtkmm/window.h>

#include <memory>
#include <utility>

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
    auto compRegistry = ComponentRegistry{};
    auto registry = ActionRegistry{compRegistry.schema()};

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

    auto session = uimodel::LayoutSession{};
    auto buildSnapshot = activateBuildSnapshot(session);
    auto ctx = LayoutBuildContext{.registry = compRegistry,
                                  .actionRegistry = registry,
                                  .parentWindow = window,
                                  .session = session,
                                  .buildSnapshot = std::move(buildSnapshot)};
    constexpr auto kAllSlots = actionSlotBit(ActionSlot::PrimaryClick) | actionSlotBit(ActionSlot::PrimaryLongPress) |
                               actionSlotBit(ActionSlot::SecondaryClick) |
                               actionSlotBit(ActionSlot::SecondaryLongPress);
    auto const allActions =
      ComponentSchema{.id = "interactive", .displayName = "Interactive", .actionSlots = kAllSlots};

    SECTION("attaches primary click to Gtk::Button")
    {
      auto button = Gtk::Button{};
      auto node = uimodel::LayoutNode{.type = "btn"};
      node.props[std::string{uimodel::kPrimaryActionProp}] = uimodel::LayoutValue{std::string{"primary"}};

      auto controller = ComponentInteractionController{};
      controller.attach(ctx, node, button, allActions);

      emitClicked(button);
      CHECK(primaryClicked);
    }

    SECTION("attaches and triggers default actions when node props are missing")
    {
      auto button = Gtk::Button{};
      auto node = uimodel::LayoutNode{.type = "btn"}; // No props

      auto schema = ComponentSchema{.id = "primary",
                                    .displayName = "Primary",
                                    .actionSlots = actionSlotBit(ActionSlot::PrimaryClick),
                                    .defaultActions = {
                                      {.slot = ActionSlot::PrimaryClick, .actionId = "primary"},
                                    }};

      auto controller = ComponentInteractionController{};
      controller.attach(ctx, node, button, schema);

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
      controller.attach(ctx, node, box, allActions);

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
      auto const secondaryActions = ComponentSchema{
        .id = "secondary",
        .displayName = "Secondary",
        .actionSlots = actionSlotBit(ActionSlot::SecondaryClick) | actionSlotBit(ActionSlot::SecondaryLongPress),
      };
      controller.attach(ctx, node, button, secondaryActions);

      emitClicked(button);
      CHECK_FALSE(primaryClicked);
    }

    SECTION("removes installed gesture controllers when destroyed before its target")
    {
      auto box = Gtk::Box{};
      auto node = uimodel::LayoutNode{.type = "box"};
      node.props[std::string{uimodel::kSecondaryActionProp}] = uimodel::LayoutValue{std::string{"secondary"}};
      node.props[std::string{uimodel::kPrimaryLongPressActionProp}] = uimodel::LayoutValue{std::string{"primaryLong"}};
      node.props[std::string{uimodel::kSecondaryLongPressActionProp}] =
        uimodel::LayoutValue{std::string{"secondaryLong"}};
      auto const initialControllerCount = box.observe_controllers()->get_n_items();

      {
        auto controllerPtr = std::make_unique<ComponentInteractionController>();
        controllerPtr->attach(ctx, node, box, allActions);
        CHECK(box.observe_controllers()->get_n_items() == initialControllerCount + 3);
      }

      CHECK(box.observe_controllers()->get_n_items() == initialControllerCount);
    }

    SECTION("keeps the bound action alive when dispatch destroys the controller")
    {
      auto button = Gtk::Button{};
      auto controllerPtr = std::make_unique<ComponentInteractionController>();
      bool actionCompleted = false;
      registry.registerAction({.id = "destroying", .label = "Destroying", .category = "Test"},
                              [&](auto&)
                              {
                                controllerPtr.reset();
                                actionCompleted = true;
                              });
      auto node = uimodel::LayoutNode{.type = "btn"};
      node.props[std::string{uimodel::kPrimaryActionProp}] = uimodel::LayoutValue{std::string{"destroying"}};
      controllerPtr->attach(ctx, node, button, allActions);

      emitClicked(button);

      CHECK(controllerPtr == nullptr);
      CHECK(actionCompleted);
    }
  }
} // namespace ao::gtk::layout::test
