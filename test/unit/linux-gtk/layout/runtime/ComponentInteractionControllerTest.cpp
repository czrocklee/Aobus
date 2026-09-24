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
#include <gdk/gdk.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/gesturelongpress.h>
#include <gtkmm/window.h>

#include <cstdint>
#include <memory>
#include <utility>

namespace ao::gtk::layout::test
{
  using namespace uimodel;
  using namespace ao::gtk::test;

  TEST_CASE("ComponentInteractionController - routes configured gestures to layout actions",
            "[gtk][unit][layout][runtime][async]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto window = Gtk::Window{};
    auto compRegistry = ComponentRegistry{};
    auto registry = ActionRegistry{compRegistry.schema()};

    std::int32_t primaryClicked = 0;
    std::int32_t secondaryClicked = 0;
    std::int32_t primaryLongPressed = 0;
    std::int32_t secondaryLongPressed = 0;

    registry.tryRegisterAction(
      {.id = "primary", .label = "Primary", .category = "Test"}, [&](auto&) { ++primaryClicked; });
    registry.tryRegisterAction(
      {.id = "secondary", .label = "Secondary", .category = "Test"}, [&](auto&) { ++secondaryClicked; });
    registry.tryRegisterAction(
      {.id = "primaryLong", .label = "Primary Long", .category = "Test"}, [&](auto&) { ++primaryLongPressed; });
    registry.tryRegisterAction(
      {.id = "secondaryLong", .label = "Secondary Long", .category = "Test"}, [&](auto&) { ++secondaryLongPressed; });

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
      CHECK(primaryClicked == 1);
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
      CHECK(primaryClicked == 1);
    }

    SECTION("secondary and long-press gestures dispatch configured actions")
    {
      auto box = Gtk::Box{};
      auto node = uimodel::LayoutNode{.type = "box"};
      node.props[std::string{uimodel::kSecondaryActionProp}] = uimodel::LayoutValue{std::string{"secondary"}};
      node.props[std::string{uimodel::kPrimaryLongPressActionProp}] = uimodel::LayoutValue{std::string{"primaryLong"}};

      auto controller = ComponentInteractionController{};
      controller.attach(ctx, node, box, allActions);

      REQUIRE(tryEmitGestureReleased(box));
      CHECK(secondaryClicked == 1);
      CHECK(primaryLongPressed == 0);

      auto const longPressPtr = findController<Gtk::GestureLongPress>(box);
      REQUIRE(longPressPtr);
      ::g_signal_emit_by_name(longPressPtr->gobj(), "pressed", 1.0, 1.0);
      CHECK(primaryLongPressed == 1);
    }

    SECTION("all four gesture slots dispatch and long press suppresses its paired click")
    {
      auto button = Gtk::Button{};
      auto node = uimodel::LayoutNode{.type = "button"};
      node.props[std::string{uimodel::kPrimaryActionProp}] = uimodel::LayoutValue{std::string{"primary"}};
      node.props[std::string{uimodel::kSecondaryActionProp}] = uimodel::LayoutValue{std::string{"secondary"}};
      node.props[std::string{uimodel::kPrimaryLongPressActionProp}] = uimodel::LayoutValue{std::string{"primaryLong"}};
      node.props[std::string{uimodel::kSecondaryLongPressActionProp}] =
        uimodel::LayoutValue{std::string{"secondaryLong"}};

      auto controller = ComponentInteractionController{};
      controller.attach(ctx, node, button, allActions);

      auto const primaryLongPtr = findControllerIf<Gtk::GestureLongPress>(
        button, [](Gtk::GestureLongPress const& gesture) { return gesture.get_button() == GDK_BUTTON_PRIMARY; });
      REQUIRE(primaryLongPtr);
      ::g_signal_emit_by_name(primaryLongPtr->gobj(), "pressed", 1.0, 1.0);
      CHECK(primaryLongPressed == 1);
      emitClicked(button);
      CHECK(primaryClicked == 0);
      emitClicked(button);
      CHECK(primaryClicked == 1);

      auto const secondaryClickPtr = findControllerIf<Gtk::GestureClick>(
        button, [](Gtk::GestureClick const& gesture) { return gesture.get_button() == GDK_BUTTON_SECONDARY; });
      auto const secondaryLongPtr = findControllerIf<Gtk::GestureLongPress>(
        button, [](Gtk::GestureLongPress const& gesture) { return gesture.get_button() == GDK_BUTTON_SECONDARY; });
      REQUIRE(secondaryClickPtr);
      REQUIRE(secondaryLongPtr);
      ::g_signal_emit_by_name(secondaryLongPtr->gobj(), "pressed", 1.0, 1.0);
      CHECK(secondaryLongPressed == 1);
      ::g_signal_emit_by_name(secondaryClickPtr->gobj(), "released", 1, 1.0, 1.0);
      CHECK(secondaryClicked == 0);
      ::g_signal_emit_by_name(secondaryClickPtr->gobj(), "released", 1, 1.0, 1.0);
      CHECK(secondaryClicked == 1);
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
      CHECK(primaryClicked == 0);
      auto const secondaryClickPtr = findControllerIf<Gtk::GestureClick>(
        button, [](Gtk::GestureClick const& gesture) { return gesture.get_button() == GDK_BUTTON_SECONDARY; });
      REQUIRE(secondaryClickPtr);
      ::g_signal_emit_by_name(secondaryClickPtr->gobj(), "released", 1, 1.0, 1.0);
      CHECK(secondaryClicked == 1);
    }

    SECTION("reattachment retires old target controls and dispatches only from the live target")
    {
      auto firstTarget = Gtk::Box{};
      auto secondTarget = Gtk::Box{};
      auto node = uimodel::LayoutNode{.type = "box"};
      node.props[std::string{uimodel::kSecondaryActionProp}] = uimodel::LayoutValue{std::string{"secondary"}};
      node.props[std::string{uimodel::kPrimaryLongPressActionProp}] = uimodel::LayoutValue{std::string{"primaryLong"}};
      node.props[std::string{uimodel::kSecondaryLongPressActionProp}] =
        uimodel::LayoutValue{std::string{"secondaryLong"}};
      auto const firstInitialCount = firstTarget.observe_controllers()->get_n_items();
      auto const secondInitialCount = secondTarget.observe_controllers()->get_n_items();

      auto controller = ComponentInteractionController{};
      controller.attach(ctx, node, firstTarget, allActions);
      CHECK(firstTarget.observe_controllers()->get_n_items() == firstInitialCount + 3);

      auto const retiredSecondaryClickPtr = findController<Gtk::GestureClick>(firstTarget);
      auto const retiredPrimaryLongPtr = findControllerIf<Gtk::GestureLongPress>(
        firstTarget, [](Gtk::GestureLongPress const& gesture) { return gesture.get_button() == GDK_BUTTON_PRIMARY; });
      auto const retiredSecondaryLongPtr = findControllerIf<Gtk::GestureLongPress>(
        firstTarget, [](Gtk::GestureLongPress const& gesture) { return gesture.get_button() == GDK_BUTTON_SECONDARY; });
      REQUIRE(retiredSecondaryClickPtr);
      REQUIRE(retiredPrimaryLongPtr);
      REQUIRE(retiredSecondaryLongPtr);

      controller.attach(ctx, node, secondTarget, allActions);
      CHECK(firstTarget.observe_controllers()->get_n_items() == firstInitialCount);
      CHECK(secondTarget.observe_controllers()->get_n_items() == secondInitialCount + 3);

      ::g_signal_emit_by_name(retiredSecondaryClickPtr->gobj(), "released", 1, 1.0, 1.0);
      ::g_signal_emit_by_name(retiredPrimaryLongPtr->gobj(), "pressed", 1.0, 1.0);
      ::g_signal_emit_by_name(retiredSecondaryLongPtr->gobj(), "pressed", 1.0, 1.0);
      CHECK(secondaryClicked == 0);
      CHECK(primaryLongPressed == 0);
      CHECK(secondaryLongPressed == 0);

      REQUIRE(tryEmitGestureReleased(secondTarget));
      auto const livePrimaryLongPtr = findControllerIf<Gtk::GestureLongPress>(
        secondTarget, [](Gtk::GestureLongPress const& gesture) { return gesture.get_button() == GDK_BUTTON_PRIMARY; });
      auto const liveSecondaryLongPtr = findControllerIf<Gtk::GestureLongPress>(
        secondTarget,
        [](Gtk::GestureLongPress const& gesture) { return gesture.get_button() == GDK_BUTTON_SECONDARY; });
      REQUIRE(livePrimaryLongPtr);
      REQUIRE(liveSecondaryLongPtr);
      ::g_signal_emit_by_name(livePrimaryLongPtr->gobj(), "pressed", 1.0, 1.0);
      ::g_signal_emit_by_name(liveSecondaryLongPtr->gobj(), "pressed", 1.0, 1.0);
      CHECK(secondaryClicked == 1);
      CHECK(primaryLongPressed == 1);
      CHECK(secondaryLongPressed == 1);
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
      registry.tryRegisterAction({.id = "destroying", .label = "Destroying", .category = "Test"},
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
