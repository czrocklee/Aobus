// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/ActionRegistry.h"

#include "test/unit/TestUtils.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/uimodel/layout/action/LayoutActionActivation.h>
#include <ao/uimodel/layout/action/LayoutActionBinding.h>
#include <ao/uimodel/layout/action/LayoutActionCapabilities.h>
#include <ao/uimodel/layout/action/LayoutActionDescriptor.h>
#include <ao/uimodel/layout/action/LayoutActionSlot.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/application.h>
#include <gtkmm/box.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#include <cstdint>

namespace ao::gtk::layout::test
{
  using namespace uimodel;
  using ao::gtk::test::makeRuntime;

  TEST_CASE("ActionRegistry - binds and dispatches layout actions", "[gtk][unit][layout][action]")
  {
    auto registry = ActionRegistry{};

    auto const descriptor1 = LayoutActionDescriptor{
      .id = "test.action1", .label = "Test Action 1", .category = "Test", .capabilities = LayoutActionCapability::None};

    auto const descriptor2 = LayoutActionDescriptor{.id = "test.action2",
                                                    .label = "Test Action 2",
                                                    .category = "Test",
                                                    .capabilities = LayoutActionCapability::RequiresAnchor};

    auto const appPtr = Gtk::Application::create("io.github.aobus.layout_test");

    auto const tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);

    auto window = Gtk::Window{};
    auto widget = Gtk::Box{};

    auto ctx = ActionActivationContext{
      .runtime = runtime, .parentWindow = window, .anchorWidget = widget, .componentId = "test_component"};

    SECTION("Registers and retrieves actions")
    {
      bool called = false;
      REQUIRE(registry.registerAction(descriptor1, [&](auto&) { called = true; }));

      auto const optDesc = registry.descriptor("test.action1");
      REQUIRE(optDesc);
      CHECK(optDesc->id == "test.action1");

      auto const all = registry.descriptors();
      REQUIRE(all.size() == 1);
      CHECK(all[0].id == "test.action1");

      CHECK(registry.activate("test.action1", ctx).result == LayoutActionActivationResult::Activated);
      CHECK(called);
    }

    SECTION("Rejects duplicate ids")
    {
      REQUIRE(registry.registerAction(descriptor1, nullptr));
      REQUIRE_FALSE(registry.registerAction(descriptor1, nullptr));

      auto const all = registry.descriptors();
      CHECK(all.size() == 1);
    }

    SECTION("canBind rejects if anchor is required but not provided")
    {
      registry.registerAction({.id = "my.test.action",
                               .label = "Test Action",
                               .category = "Test",
                               .capabilities = LayoutActionCapability::RequiresAnchor},
                              [](auto&) {});

      CHECK(!registry.canBind(
        "my.test.action",
        LayoutActionBindingContext{
          .slot = LayoutActionSlot::PrimaryClick, .hasAnchor = false, .hasFocusedView = false, .componentType = {}}));
      CHECK(registry.canBind(
        "my.test.action",
        LayoutActionBindingContext{
          .slot = LayoutActionSlot::PrimaryClick, .hasAnchor = true, .hasFocusedView = false, .componentType = {}}));
    }

    SECTION("canBind returns false for unknown action")
    {
      CHECK(!registry.canBind(
        "unknown.action",
        LayoutActionBindingContext{
          .slot = LayoutActionSlot::PrimaryClick, .hasAnchor = false, .hasFocusedView = false, .componentType = {}}));
    }

    SECTION("Distinguishes canBind() from runtime state()")
    {
      REQUIRE(registry.registerAction(descriptor2, nullptr));

      // Requires anchor, so cannot bind to shortcut
      CHECK_FALSE(registry.canBind(
        "test.action2",
        LayoutActionBindingContext{
          .slot = LayoutActionSlot::Shortcut, .hasAnchor = false, .hasFocusedView = false, .componentType = {}}));
      CHECK(registry.canBind(
        "test.action2",
        LayoutActionBindingContext{
          .slot = LayoutActionSlot::PrimaryClick, .hasAnchor = true, .hasFocusedView = false, .componentType = {}}));

      // Default state is enabled
      auto const s = registry.state("test.action2", ctx);
      CHECK(s.enabled);
    }

    SECTION("Activates handlers with context and returns Activated")
    {
      bool called = false;
      registry.registerAction(descriptor1,
                              [&](ActionActivationContext const& c)
                              {
                                called = true;
                                CHECK(c.componentId == "test_component");
                              });

      auto const outcome = registry.activate("test.action1", ctx);
      CHECK(outcome.result == LayoutActionActivationResult::Activated);
      CHECK(called);
    }

    SECTION("Does not dispatch disabled actions and returns Disabled")
    {
      bool called = false;
      registry.registerAction(
        descriptor1,
        [&](auto&) { called = true; },
        [](auto const&) { return LayoutActionAvailability{.enabled = false, .disabledReason = "Test"}; });

      auto const s = registry.state("test.action1", ctx);
      CHECK_FALSE(s.enabled);
      CHECK(s.disabledReason == "Test");

      auto const outcome = registry.activate("test.action1", ctx);
      CHECK(outcome.result == LayoutActionActivationResult::Disabled);
      CHECK_FALSE(called);
    }

    SECTION("Empty registry returns no descriptors and unknown id lookup returns nullopt")
    {
      CHECK(registry.descriptors().empty());
      CHECK_FALSE(registry.descriptor("unknown"));
      CHECK_FALSE(registry.canBind(
        "unknown",
        LayoutActionBindingContext{
          .slot = LayoutActionSlot::PrimaryClick, .hasAnchor = false, .hasFocusedView = false, .componentType = {}}));
    }

    SECTION("Activating an unknown action id returns UnknownAction")
    {
      auto const outcome = registry.activate("unknown", ctx);
      CHECK(outcome.result == LayoutActionActivationResult::UnknownAction);
    }

    SECTION("State provider is called during activate() to gate dispatch")
    {
      std::int32_t stateCalls = 0;
      registry.registerAction(
        descriptor1,
        [](auto&) {},
        [&](auto const&)
        {
          stateCalls++;
          return LayoutActionAvailability{.enabled = true, .disabledReason = ""};
        });

      registry.activate("test.action1", ctx);
      CHECK(stateCalls == 1);
    }
  }
} // namespace ao::gtk::layout::test
