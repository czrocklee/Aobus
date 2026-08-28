// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/ActionRegistry.h"

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include <ao/rt/AppRuntime.h>
#include <ao/uimodel/layout/component/LayoutSchema.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/application.h>
#include <gtkmm/box.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#include <cstdint>
#include <memory>

namespace ao::gtk::layout::test
{
  using namespace uimodel;
  using ao::gtk::test::makeRuntime;

  TEST_CASE("ActionRegistry - binds and dispatches layout actions", "[gtk][unit][layout][action]")
  {
    auto schema = LayoutSchema{};
    auto registry = ActionRegistry{schema};

    auto const actionSchema =
      ActionSchema{.id = "test.action1", .label = "Test Action 1", .category = "Test", .capabilities = 0};

    auto const appPtr = Gtk::Application::create("io.github.aobus.layout_test");

    auto const tempDir = ao::test::TempDir{};
    std::unique_ptr<rt::AppRuntime> const runtimePtr = makeRuntime(tempDir);

    auto window = Gtk::Window{};
    auto widget = Gtk::Box{};

    auto ctx = ActionActivationContext{.parentWindow = window, .anchorWidget = widget, .componentId = "test_component"};

    SECTION("Registers and retrieves actions")
    {
      bool called = false;
      REQUIRE(registry.registerAction(actionSchema, [&](auto&) { called = true; }));

      auto const optActionSchema = registry.action("test.action1");
      REQUIRE(optActionSchema);
      CHECK(optActionSchema->id == "test.action1");

      auto const all = registry.actions();
      REQUIRE(all.size() == 1);
      CHECK(all[0].id == "test.action1");

      CHECK(registry.activate("test.action1", ctx));
      CHECK(called);
    }

    SECTION("Rejects duplicate ids")
    {
      REQUIRE(registry.registerAction(actionSchema, nullptr));
      REQUIRE_FALSE(registry.registerAction(actionSchema, nullptr));

      auto const all = registry.actions();
      CHECK(all.size() == 1);
    }

    SECTION("Activates handlers with context")
    {
      bool called = false;
      registry.registerAction(actionSchema,
                              [&](ActionActivationContext const& c)
                              {
                                called = true;
                                CHECK(c.componentId == "test_component");
                              });

      CHECK(registry.activate("test.action1", ctx));
      CHECK(called);
    }

    SECTION("Does not dispatch disabled actions")
    {
      bool called = false;
      registry.registerAction(
        actionSchema,
        [&](auto&) { called = true; },
        [](auto const&) { return ActionAvailability{.enabled = false, .disabledReason = "Test"}; });

      auto const s = registry.state("test.action1", ctx);
      CHECK_FALSE(s.enabled);
      CHECK(s.disabledReason == "Test");

      CHECK_FALSE(registry.activate("test.action1", ctx));
      CHECK_FALSE(called);
    }

    SECTION("Empty registry returns no schema entries and unknown id lookup returns nullopt")
    {
      CHECK(registry.actions().empty());
      CHECK_FALSE(registry.action("unknown"));
    }

    SECTION("Activating an unknown action id returns false")
    {
      CHECK_FALSE(registry.activate("unknown", ctx));
    }

    SECTION("State provider is called during activate() to gate dispatch")
    {
      std::int32_t stateCalls = 0;
      registry.registerAction(
        actionSchema,
        [](auto&) {},
        [&](auto const&)
        {
          stateCalls++;
          return ActionAvailability{.enabled = true, .disabledReason = ""};
        });

      registry.activate("test.action1", ctx);
      CHECK(stateCalls == 1);
    }
  }
} // namespace ao::gtk::layout::test
