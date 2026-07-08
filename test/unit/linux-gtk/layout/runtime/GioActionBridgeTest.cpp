// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/GioActionBridge.h"

#include "layout/runtime/ActionRegistry.h"
#include "test/unit/TestUtils.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/rt/AppRuntime.h>
#include <ao/uimodel/layout/action/LayoutActionAvailability.h>
#include <ao/uimodel/layout/action/LayoutActionCapabilities.h>
#include <ao/uimodel/layout/action/LayoutActionDescriptor.h>

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
    class DummyContextProvider final : public ActionContextProvider
    {
    public:
      DummyContextProvider(rt::AppRuntime& runtime, Gtk::Window& window, Gtk::Widget& widget)
        : _runtime{runtime}, _window{window}, _widget{widget}
      {
      }

      ActionActivationContext actionContext(std::string_view componentId) override
      {
        return ActionActivationContext{.runtime = _runtime,
                                       .parentWindow = _window,
                                       .anchorWidget = _widget,
                                       .componentId = std::string{componentId}};
      }

      bool canProvideSafeAnchor(LayoutActionDescriptor const& /*desc*/) const override { return _canProvideSafeAnchor; }

      void setCanProvideSafeAnchor(bool val) { _canProvideSafeAnchor = val; }

    private:
      rt::AppRuntime& _runtime;
      Gtk::Window& _window;
      Gtk::Widget& _widget;
      bool _canProvideSafeAnchor = false;
    };
  } // namespace

  TEST_CASE("GioActionBridge - exports layout actions to Gio action maps", "[gtk][unit][layout][action]")
  {
    auto const appPtr = Gtk::Application::create("io.github.aobus.layout_test.gio");
    auto const tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);

    auto window = Gtk::Window{};
    auto widget = Gtk::Box{};
    auto contextProvider = DummyContextProvider{runtime, window, widget};

    auto registry = ActionRegistry{};
    auto actionMapPtr = Gio::SimpleActionGroup::create();

    SECTION("Exports pure command actions")
    {
      std::int32_t action1Fired = 0;
      registry.registerAction(
        LayoutActionDescriptor{
          .id = "test.action1", .label = "Action 1", .category = "Test", .capabilities = LayoutActionCapability::None},
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
      registry.registerAction(LayoutActionDescriptor{.id = "test.action2",
                                                     .label = "Action 2",
                                                     .category = "Test",
                                                     .capabilities = LayoutActionCapability::RequiresAnchor},
                              [&](ActionActivationContext&) {});

      GioActionBridge::exportActions(registry, *actionMapPtr, contextProvider);

      auto gioActionPtr = actionMapPtr->lookup_action("test.action2");
      CHECK(gioActionPtr == nullptr);
    }

    SECTION("Does not export menu-presenting actions if no safe anchor")
    {
      registry.registerAction(LayoutActionDescriptor{.id = "test.action3",
                                                     .label = "Action 3",
                                                     .category = "Test",
                                                     .capabilities = LayoutActionCapability::PresentsMenu},
                              [&](ActionActivationContext&) {});

      GioActionBridge::exportActions(registry, *actionMapPtr, contextProvider);

      auto gioActionPtr = actionMapPtr->lookup_action("test.action3");
      CHECK(gioActionPtr == nullptr);
    }

    SECTION("Exports anchored actions if safe anchor is provided")
    {
      contextProvider.setCanProvideSafeAnchor(true);

      registry.registerAction(LayoutActionDescriptor{.id = "test.action_anchored",
                                                     .label = "Anchored Action",
                                                     .category = "Test",
                                                     .capabilities = LayoutActionCapability::RequiresAnchor},
                              [&](ActionActivationContext&) {});

      GioActionBridge::exportActions(registry, *actionMapPtr, contextProvider);

      auto gioActionPtr = actionMapPtr->lookup_action("test.action_anchored");
      CHECK(gioActionPtr != nullptr);
    }

    SECTION("refreshStates updates enabled state of exported actions")
    {
      bool isEnabled = true;
      registry.registerAction(
        LayoutActionDescriptor{.id = "test.action_refresh",
                               .label = "Refresh Action",
                               .category = "Test",
                               .capabilities = LayoutActionCapability::None},
        [&](ActionActivationContext&) {},
        [&](ActionActivationContext const&)
        { return LayoutActionAvailability{.enabled = isEnabled, .disabledReason = ""}; });

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
