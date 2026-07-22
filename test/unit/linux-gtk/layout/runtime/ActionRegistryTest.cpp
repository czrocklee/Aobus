// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/ActionRegistry.h"

#include "test/unit/TestUtils.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/uimodel/layout/action/LayoutActionAvailability.h>
#include <ao/uimodel/layout/action/LayoutActionCapabilities.h>
#include <ao/uimodel/layout/action/LayoutActionDescriptor.h>

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

      CHECK(registry.activate("test.action1", ctx));
      CHECK(called);
    }

    SECTION("Rejects duplicate ids")
    {
      REQUIRE(registry.registerAction(descriptor1, nullptr));
      REQUIRE_FALSE(registry.registerAction(descriptor1, nullptr));

      auto const all = registry.descriptors();
      CHECK(all.size() == 1);
    }

    SECTION("Activates handlers with context")
    {
      bool called = false;
      registry.registerAction(descriptor1,
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
        descriptor1,
        [&](auto&) { called = true; },
        [](auto const&) { return LayoutActionAvailability{.enabled = false, .disabledReason = "Test"}; });

      auto const s = registry.state("test.action1", ctx);
      CHECK_FALSE(s.enabled);
      CHECK(s.disabledReason == "Test");

      CHECK_FALSE(registry.activate("test.action1", ctx));
      CHECK_FALSE(called);
    }

    SECTION("Empty registry returns no descriptors and unknown id lookup returns nullopt")
    {
      CHECK(registry.descriptors().empty());
      CHECK_FALSE(registry.descriptor("unknown"));
    }

    SECTION("Activating an unknown action id returns false")
    {
      CHECK_FALSE(registry.activate("unknown", ctx));
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
