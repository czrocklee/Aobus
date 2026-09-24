// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/GioActionBridge.h"

#include "layout/runtime/ActionRegistry.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
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
        ++_contextCalls;
        return ActionActivationContext{
          .parentWindow = _window, .anchorWidget = _widget, .componentId = std::string{componentId}};
      }

      bool canProvideSafeAnchor(ActionSchema const& /*actionSchema*/) const override { return _canProvideSafeAnchor; }

      void setCanProvideSafeAnchor(bool val) { _canProvideSafeAnchor = val; }
      std::int32_t contextCalls() const { return _contextCalls; }

    private:
      Gtk::Window& _window;
      Gtk::Widget& _widget;
      bool _canProvideSafeAnchor = false;
      std::int32_t _contextCalls = 0;
    };

    struct GioActionBridgeFixture final
    {
      Glib::RefPtr<Gtk::Application> appPtr = ao::gtk::test::ensureGtkApplication();
      Gtk::Window window{};
      Gtk::Box widget{};
      FakeActionContextProvider contextProvider{window, widget};
      LayoutSchema schema;
      ActionRegistry registry{schema};
      Glib::RefPtr<Gio::SimpleActionGroup> actionMapPtr = Gio::SimpleActionGroup::create();
    };
  } // namespace

  TEST_CASE("GioActionBridge - exports actions with the required context capabilities", "[gtk][unit][layout][action]")
  {
    auto fixture = GioActionBridgeFixture{};
    auto& window = fixture.window;
    auto& widget = fixture.widget;
    auto& contextProvider = fixture.contextProvider;
    auto& registry = fixture.registry;
    auto const& actionMapPtr = fixture.actionMapPtr;

    SECTION("Exports pure command actions")
    {
      std::int32_t action1Fired = 0;
      auto componentId = std::string{};
      Gtk::Window* parentWindow = nullptr;
      Gtk::Widget* anchorWidget = nullptr;
      REQUIRE(registry.tryRegisterAction(
        ActionSchema{.id = "test.action1", .label = "Action 1", .category = "Test", .capabilities = 0},
        [&](ActionActivationContext& ctx)
        {
          ++action1Fired;
          componentId = ctx.componentId;
          parentWindow = &ctx.parentWindow;
          anchorWidget = &ctx.anchorWidget;
        }));

      [[maybe_unused]] auto session = GioActionBridge::exportActions(registry, *actionMapPtr, contextProvider);

      auto gioActionPtr = actionMapPtr->lookup_action("test.action1");
      REQUIRE(gioActionPtr != nullptr);

      // Trigger Gio action
      actionMapPtr->activate_action("test.action1");
      CHECK(action1Fired == 1);
      CHECK(componentId == "test.action1");
      CHECK(parentWindow == &window);
      CHECK(anchorWidget == &widget);
    }

    SECTION("Does not export anchored actions if no safe anchor")
    {
      registry.tryRegisterAction(ActionSchema{.id = "test.action2",
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
      registry.tryRegisterAction(ActionSchema{.id = "test.action3",
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

      std::int32_t activations = 0;
      Gtk::Widget* activatedAnchor = nullptr;
      REQUIRE(
        registry.tryRegisterAction(ActionSchema{.id = "test.action_anchored",
                                                .label = "Anchored Action",
                                                .category = "Test",
                                                .capabilities = actionCapabilityBit(ActionCapability::RequiresAnchor)},
                                   [&](ActionActivationContext& ctx)
                                   {
                                     ++activations;
                                     activatedAnchor = &ctx.anchorWidget;
                                   }));

      [[maybe_unused]] auto session = GioActionBridge::exportActions(registry, *actionMapPtr, contextProvider);

      auto gioActionPtr = actionMapPtr->lookup_action("test.action_anchored");
      REQUIRE(gioActionPtr != nullptr);
      actionMapPtr->activate_action("test.action_anchored");
      CHECK(activations == 1);
      CHECK(activatedAnchor == &widget);
    }
  }

  TEST_CASE("GioActionBridge - refreshes enabled state and live activation", "[gtk][unit][layout][action]")
  {
    auto fixture = GioActionBridgeFixture{};
    auto& contextProvider = fixture.contextProvider;
    auto& registry = fixture.registry;
    auto const& actionMapPtr = fixture.actionMapPtr;

    SECTION("refreshStates updates enabled state of exported actions")
    {
      bool isEnabled = true;
      std::int32_t activations = 0;
      registry.tryRegisterAction(
        ActionSchema{.id = "test.action_refresh", .label = "Refresh Action", .category = "Test", .capabilities = 0},
        [&](ActionActivationContext&) { ++activations; },
        [&](ActionActivationContext const&) { return ActionAvailability{.enabled = isEnabled, .disabledReason = ""}; });

      auto session = GioActionBridge::exportActions(registry, *actionMapPtr, contextProvider);

      auto gioActionPtr = actionMapPtr->lookup_action("test.action_refresh");
      REQUIRE(gioActionPtr != nullptr);
      CHECK(gioActionPtr->property_enabled() == true);
      actionMapPtr->activate_action("test.action_refresh");
      CHECK(activations == 1);

      // Change state and refresh
      isEnabled = false;
      session.refreshStates();
      CHECK(gioActionPtr->property_enabled() == false);
      actionMapPtr->activate_action("test.action_refresh");
      CHECK(activations == 1);

      isEnabled = true;
      session.refreshStates();
      CHECK(gioActionPtr->property_enabled() == true);
      actionMapPtr->activate_action("test.action_refresh");
      CHECK(activations == 2);
    }
  }

  TEST_CASE("GioActionBridge - owns export retirement and partial rollback", "[gtk][unit][layout][action][async]")
  {
    auto fixture = GioActionBridgeFixture{};
    auto& contextProvider = fixture.contextProvider;
    auto& registry = fixture.registry;
    auto const& actionMapPtr = fixture.actionMapPtr;

    SECTION("session teardown unexports actions and revokes retained activation")
    {
      std::int32_t activationCount = 0;
      registry.tryRegisterAction(
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
      registry.tryRegisterAction(
        ActionSchema{.id = "test.moved", .label = "Moved", .category = "Test", .capabilities = 0},
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
      registry.tryRegisterAction(
        ActionSchema{.id = "test.replaced", .label = "Replaced", .category = "Test", .capabilities = 0},
        [&oldActivationCount](ActionActivationContext&) { ++oldActivationCount; },
        [&isEnabled](ActionActivationContext const&)
        { return ActionAvailability{.enabled = isEnabled, .disabledReason = ""}; });

      auto optSession =
        std::optional<GioActionBridgeSession>{GioActionBridge::exportActions(registry, *actionMapPtr, contextProvider)};
      auto retainedOldActionPtr =
        std::dynamic_pointer_cast<Gio::SimpleAction>(actionMapPtr->lookup_action("test.replaced"));
      REQUIRE(retainedOldActionPtr);
      retainedOldActionPtr->activate();
      CHECK(oldActivationCount == 1);

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

      // Availability must not mask a callback that wrongly survives retirement.
      isEnabled = true;
      auto const callsBeforeRetiredActivation = contextProvider.contextCalls();
      retainedOldActionPtr->activate();
      CHECK(contextProvider.contextCalls() == callsBeforeRetiredActivation);
      CHECK(oldActivationCount == 1);
      replacementActionPtr->activate();
      CHECK(replacementActivationCount == 1);
    }

    SECTION("failed partial export rolls back actions already installed")
    {
      registry.tryRegisterAction(
        ActionSchema{.id = "test.first", .label = "First", .category = "Test", .capabilities = 0},
        [](ActionActivationContext&) {});
      bool firstInstalledBeforeFailure = false;
      registry.tryRegisterAction(
        ActionSchema{.id = "test.second", .label = "Second", .category = "Test", .capabilities = 0},
        [](ActionActivationContext&) {},
        [&](ActionActivationContext const&) -> ActionAvailability
        {
          firstInstalledBeforeFailure = actionMapPtr->lookup_action("test.first") != nullptr;
          throw std::runtime_error{"state failure"};
        });

      auto exportFailingSession = [&]
      { std::ignore = GioActionBridge::exportActions(registry, *actionMapPtr, contextProvider); };
      REQUIRE_THROWS_AS(exportFailingSession(), std::runtime_error);
      CHECK(firstInstalledBeforeFailure);
      CHECK(actionMapPtr->lookup_action("test.first") == nullptr);
      CHECK(actionMapPtr->lookup_action("test.second") == nullptr);
    }

    SECTION("skipped anchored action leaves a foreign action untouched")
    {
      registry.tryRegisterAction(ActionSchema{.id = "test.foreign",
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
