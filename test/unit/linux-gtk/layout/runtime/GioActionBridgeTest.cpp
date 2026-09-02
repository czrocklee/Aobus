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
#include <sigc++/scoped_connection.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

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

      [[maybe_unused]] auto session = GioActionBridge::exportActions(registry, *actionMapPtr, contextProvider);

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

      [[maybe_unused]] auto session = GioActionBridge::exportActions(registry, *actionMapPtr, contextProvider);

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

      [[maybe_unused]] auto session = GioActionBridge::exportActions(registry, *actionMapPtr, contextProvider);

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

      [[maybe_unused]] auto session = GioActionBridge::exportActions(registry, *actionMapPtr, contextProvider);

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

      auto session = GioActionBridge::exportActions(registry, *actionMapPtr, contextProvider);

      auto gioActionPtr = actionMapPtr->lookup_action("test.action_refresh");
      REQUIRE(gioActionPtr != nullptr);
      CHECK(gioActionPtr->property_enabled() == true);

      // Change state and refresh
      isEnabled = false;
      session.refreshStates();
      CHECK(gioActionPtr->property_enabled() == false);
    }

    SECTION("session teardown unexports actions and revokes retained activation")
    {
      std::int32_t activationCount = 0;
      registry.registerAction(
        ActionSchema{.id = "test.retained", .label = "Retained", .category = "Test", .capabilities = 0},
        [&activationCount](ActionActivationContext&) { ++activationCount; });

      auto optSession =
        std::optional<GioActionBridgeSession>{GioActionBridge::exportActions(registry, *actionMapPtr, contextProvider)};
      auto retainedActionPtr =
        std::dynamic_pointer_cast<Gio::SimpleAction>(actionMapPtr->lookup_action("test.retained"));
      REQUIRE(retainedActionPtr);
      retainedActionPtr->activate();
      CHECK(activationCount == 1);

      optSession.reset();
      CHECK(actionMapPtr->lookup_action("test.retained") == nullptr);

      retainedActionPtr->activate();
      CHECK(activationCount == 1);
    }

    SECTION("moving a session preserves wiring and leaves the source inert")
    {
      std::int32_t activationCount = 0;
      registry.registerAction(ActionSchema{.id = "test.moved", .label = "Moved", .category = "Test", .capabilities = 0},
                              [&activationCount](ActionActivationContext&) { ++activationCount; });

      auto retainedActionPtr = Glib::RefPtr<Gio::SimpleAction>{};
      auto optMovedSession = std::optional<GioActionBridgeSession>{};
      {
        auto session = GioActionBridge::exportActions(registry, *actionMapPtr, contextProvider);
        optMovedSession.emplace(std::move(session));
        retainedActionPtr = std::dynamic_pointer_cast<Gio::SimpleAction>(actionMapPtr->lookup_action("test.moved"));
        REQUIRE(retainedActionPtr);
        retainedActionPtr->activate();
        CHECK(activationCount == 1);
      }

      REQUIRE(actionMapPtr->lookup_action("test.moved") != nullptr);
      retainedActionPtr->activate();
      CHECK(activationCount == 2);

      optMovedSession.reset();
      CHECK(actionMapPtr->lookup_action("test.moved") == nullptr);
      retainedActionPtr->activate();
      CHECK(activationCount == 2);
    }

    SECTION("replacement action survives old-session refresh and teardown")
    {
      bool isEnabled = true;
      std::int32_t oldActivationCount = 0;
      std::int32_t replacementActivationCount = 0;
      registry.registerAction(
        ActionSchema{.id = "test.replaced", .label = "Replaced", .category = "Test", .capabilities = 0},
        [&oldActivationCount](ActionActivationContext&) { ++oldActivationCount; },
        [&isEnabled](ActionActivationContext const&)
        { return ActionAvailability{.enabled = isEnabled, .disabledReason = ""}; });

      auto optSession =
        std::optional<GioActionBridgeSession>{GioActionBridge::exportActions(registry, *actionMapPtr, contextProvider)};
      auto retainedOldActionPtr =
        std::dynamic_pointer_cast<Gio::SimpleAction>(actionMapPtr->lookup_action("test.replaced"));
      REQUIRE(retainedOldActionPtr);

      auto replacementActionPtr = Gio::SimpleAction::create("test.replaced");
      auto replacementConnection = sigc::scoped_connection{replacementActionPtr->signal_activate().connect(
        [&replacementActivationCount](Glib::VariantBase const&) { ++replacementActivationCount; })};
      actionMapPtr->add_action(replacementActionPtr);

      isEnabled = false;
      optSession->refreshStates();
      CHECK(replacementActionPtr->get_enabled());

      optSession.reset();
      auto const currentActionPtr = actionMapPtr->lookup_action("test.replaced");
      REQUIRE(currentActionPtr);
      CHECK(currentActionPtr.get() == replacementActionPtr.get());

      retainedOldActionPtr->activate();
      CHECK(oldActivationCount == 0);
      replacementActionPtr->activate();
      CHECK(replacementActivationCount == 1);
    }

    SECTION("failed partial export rolls back actions already installed")
    {
      registry.registerAction(ActionSchema{.id = "test.first", .label = "First", .category = "Test", .capabilities = 0},
                              [](ActionActivationContext&) {});
      registry.registerAction(
        ActionSchema{.id = "test.second", .label = "Second", .category = "Test", .capabilities = 0},
        [](ActionActivationContext&) {},
        [](ActionActivationContext const&) -> ActionAvailability { throw std::runtime_error{"state failure"}; });

      auto exportFailingSession = [&]
      { std::ignore = GioActionBridge::exportActions(registry, *actionMapPtr, contextProvider); };
      REQUIRE_THROWS_AS(exportFailingSession(), std::runtime_error);
      CHECK(actionMapPtr->lookup_action("test.first") == nullptr);
      CHECK(actionMapPtr->lookup_action("test.second") == nullptr);
    }

    SECTION("skipped anchored action leaves a foreign action untouched")
    {
      registry.registerAction(ActionSchema{.id = "test.foreign",
                                           .label = "Foreign",
                                           .category = "Test",
                                           .capabilities = actionCapabilityBit(ActionCapability::RequiresAnchor)},
                              [](ActionActivationContext&) {});
      auto foreignActionPtr = Gio::SimpleAction::create("test.foreign");
      actionMapPtr->add_action(foreignActionPtr);

      auto optSession =
        std::optional<GioActionBridgeSession>{GioActionBridge::exportActions(registry, *actionMapPtr, contextProvider)};
      optSession.reset();

      auto const currentActionPtr = actionMapPtr->lookup_action("test.foreign");
      REQUIRE(currentActionPtr);
      CHECK(currentActionPtr.get() == foreignActionPtr.get());
    }
  }
} // namespace ao::gtk::layout::test
