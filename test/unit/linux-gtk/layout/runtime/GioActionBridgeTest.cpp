// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/GioActionBridge.h"

#include "layout/runtime/ActionRegistry.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include <ao/uimodel/layout/component/LayoutSchema.h>

#include <catch2/catch_test_macros.hpp>
#include <giomm/simpleactiongroup.h>
#include <gtkmm/application.h>
#include <gtkmm/box.h>
#include <gtkmm/window.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace ao::gtk::layout::test
{
  using namespace uimodel;
  using ao::gtk::test::makeRuntime;

  namespace
  {
    class FakeActionContextProvider final : public ActionContextProvider
    {
    public:
      FakeActionContextProvider(Gtk::Window& window, Gtk::Widget& widget)
        : _window{window}, _widget{widget}
      {
      }

      ActionActivationContext actionContext(std::string_view componentId) override
      {
        return ActionActivationContext{
          .parentWindow = _window, .anchorWidget = _widget, .componentId = std::string{componentId}};
      }

      bool canProvideSafeAnchor(ActionSchema const& /*actionSchema*/) const override { return _canProvideSafeAnchor; }

      void setCanProvideSafeAnchor(bool val) { _canProvideSafeAnchor = val; }

    private:
      Gtk::Window& _window;
      Gtk::Widget& _widget;
      bool _canProvideSafeAnchor = false;
    };
  } // namespace

  TEST_CASE("GioActionBridge - exports layout actions to Gio action maps", "[gtk][unit][layout][action]")
  {
    auto const appPtr = Gtk::Application::create("io.github.aobus.layout_test.gio");
    auto const tempDir = ao::test::TempDir{};
    auto runtimePtr = makeRuntime(tempDir);

    auto window = Gtk::Window{};
    auto widget = Gtk::Box{};
    auto contextProvider = FakeActionContextProvider{window, widget};

    auto schema = LayoutSchema{};
    auto registry = ActionRegistry{schema};
    auto actionMapPtr = Gio::SimpleActionGroup::create();

    SECTION("Exports pure command actions")
    {
      std::int32_t action1Fired = 0;
      registry.registerAction(
        ActionSchema{.id = "test.action1", .label = "Action 1", .category = "Test", .capabilities = 0},
        [&](ActionActivationContext&) { action1Fired++; });

      GioActionBridge::exportActions(registry, *actionMapPtr, contextProvider);

      auto gioActionPtr = actionMapPtr->lookup_action("test.action1");
      REQUIRE(gioActionPtr != nullptr);

      // Trigger Gio action
      actionMapPtr->activate_action("test.action1");
      CHECK(action1Fired == 1);
    }

    SECTION("Does not export anchored actions if no safe anchor")
    {
      registry.registerAction(ActionSchema{.id = "test.action2",
                                           .label = "Action 2",
                                           .category = "Test",
                                           .capabilities = actionCapabilityBit(ActionCapability::RequiresAnchor)},
                              [&](ActionActivationContext&) {});

      GioActionBridge::exportActions(registry, *actionMapPtr, contextProvider);

      auto gioActionPtr = actionMapPtr->lookup_action("test.action2");
      CHECK(gioActionPtr == nullptr);
    }

    SECTION("Does not export menu-presenting actions if no safe anchor")
    {
      registry.registerAction(ActionSchema{.id = "test.action3",
                                           .label = "Action 3",
                                           .category = "Test",
                                           .capabilities = actionCapabilityBit(ActionCapability::PresentsMenu)},
                              [&](ActionActivationContext&) {});

      GioActionBridge::exportActions(registry, *actionMapPtr, contextProvider);

      auto gioActionPtr = actionMapPtr->lookup_action("test.action3");
      CHECK(gioActionPtr == nullptr);
    }

    SECTION("Exports anchored actions if safe anchor is provided")
    {
      contextProvider.setCanProvideSafeAnchor(true);

      registry.registerAction(ActionSchema{.id = "test.action_anchored",
                                           .label = "Anchored Action",
                                           .category = "Test",
                                           .capabilities = actionCapabilityBit(ActionCapability::RequiresAnchor)},
                              [&](ActionActivationContext&) {});

      GioActionBridge::exportActions(registry, *actionMapPtr, contextProvider);

      auto gioActionPtr = actionMapPtr->lookup_action("test.action_anchored");
      CHECK(gioActionPtr != nullptr);
    }

    SECTION("refreshStates updates enabled state of exported actions")
    {
      bool isEnabled = true;
      registry.registerAction(
        ActionSchema{.id = "test.action_refresh", .label = "Refresh Action", .category = "Test", .capabilities = 0},
        [&](ActionActivationContext&) {},
        [&](ActionActivationContext const&) { return ActionAvailability{.enabled = isEnabled, .disabledReason = ""}; });

      auto sessionPtr = GioActionBridge::exportActions(registry, *actionMapPtr, contextProvider);
      REQUIRE(sessionPtr != nullptr);

      auto gioActionPtr = actionMapPtr->lookup_action("test.action_refresh");
      REQUIRE(gioActionPtr != nullptr);
      CHECK(gioActionPtr->property_enabled() == true);

      // Change state and refresh
      isEnabled = false;
      sessionPtr->refreshStates();
      CHECK(gioActionPtr->property_enabled() == false);
    }
  }
} // namespace ao::gtk::layout::test
