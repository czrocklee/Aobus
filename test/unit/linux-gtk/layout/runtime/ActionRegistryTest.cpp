// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/ActionRegistry.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"
#include "test/unit/lmdb/TestUtils.h"
#include <ao/uimodel/layout/ActionTypes.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/application.h>
#include <gtkmm/box.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#include <cstdint>

namespace ao::gtk::layout::test
{
  using namespace uimodel::layout;
  using namespace ao::lmdb::test;
  using ao::gtk::test::makeRuntime;

  TEST_CASE("ActionRegistry", "[layout][action]")
  {
    auto registry = ActionRegistry{};

    auto const descriptor1 = ActionDescriptor{
      .id = "test.action1", .label = "Test Action 1", .category = "Test", .capabilities = ActionCapability::None};

    auto const descriptor2 = ActionDescriptor{.id = "test.action2",
                                              .label = "Test Action 2",
                                              .category = "Test",
                                              .capabilities = ActionCapability::RequiresAnchor};

    auto const appPtr = Gtk::Application::create("io.github.aobus.layout_test");

    auto const tempDir = TempDir{};
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

      CHECK(registry.activate("test.action1", ctx).result == ActionActivationResult::Activated);
      CHECK(called);
    }

    SECTION("Rejects duplicate ids")
    {
      REQUIRE(registry.registerAction(descriptor1, nullptr));
      REQUIRE_FALSE(registry.registerAction(descriptor1, nullptr));

      auto const all = registry.descriptors();
      REQUIRE(all.size() == 1);
    }

    SECTION("canBind rejects if anchor is required but not provided")
    {
      registry.registerAction({.id = "my.test.action",
                               .label = "Test Action",
                               .category = "Test",
                               .capabilities = ActionCapability::RequiresAnchor},
                              [](auto&) {});

      CHECK(!registry.canBind(
        "my.test.action",
        ActionBindingContext{
          .slot = ActionSlot::PrimaryClick, .hasAnchor = false, .hasFocusedView = false, .componentType = {}}));
      CHECK(registry.canBind(
        "my.test.action",
        ActionBindingContext{
          .slot = ActionSlot::PrimaryClick, .hasAnchor = true, .hasFocusedView = false, .componentType = {}}));
    }

    SECTION("canBind returns false for unknown action")
    {
      CHECK(!registry.canBind(
        "unknown.action",
        ActionBindingContext{
          .slot = ActionSlot::PrimaryClick, .hasAnchor = false, .hasFocusedView = false, .componentType = {}}));
    }

    SECTION("Distinguishes canBind() from runtime state()")
    {
      REQUIRE(registry.registerAction(descriptor2, nullptr));

      // Requires anchor, so cannot bind to shortcut
      CHECK_FALSE(registry.canBind(
        "test.action2",
        ActionBindingContext{
          .slot = ActionSlot::Shortcut, .hasAnchor = false, .hasFocusedView = false, .componentType = {}}));
      CHECK(registry.canBind(
        "test.action2",
        ActionBindingContext{
          .slot = ActionSlot::PrimaryClick, .hasAnchor = true, .hasFocusedView = false, .componentType = {}}));

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
      CHECK(outcome.result == ActionActivationResult::Activated);
      CHECK(called);
    }

    SECTION("Does not dispatch disabled actions and returns Disabled")
    {
      bool called = false;
      registry.registerAction(
        descriptor1,
        [&](auto&) { called = true; },
        [](auto const&) { return ActionState{.enabled = false, .disabledReason = "Test"}; });

      auto const s = registry.state("test.action1", ctx);
      CHECK_FALSE(s.enabled);
      CHECK(s.disabledReason == "Test");

      auto const outcome = registry.activate("test.action1", ctx);
      CHECK(outcome.result == ActionActivationResult::Disabled);
      CHECK_FALSE(called);
    }

    SECTION("Empty registry returns no descriptors and unknown id lookup returns nullopt")
    {
      CHECK(registry.descriptors().empty());
      CHECK_FALSE(registry.descriptor("unknown"));
      CHECK_FALSE(registry.canBind(
        "unknown",
        ActionBindingContext{
          .slot = ActionSlot::PrimaryClick, .hasAnchor = false, .hasFocusedView = false, .componentType = {}}));
    }

    SECTION("Activating an unknown action id returns UnknownAction")
    {
      auto const outcome = registry.activate("unknown", ctx);
      CHECK(outcome.result == ActionActivationResult::UnknownAction);
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
          return ActionState{.enabled = true, .disabledReason = ""};
        });

      registry.activate("test.action1", ctx);
      CHECK(stateCalls == 1);
    }
  }
} // namespace ao::gtk::layout::test
