// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/ActionRegistry.h"

#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include <ao/uimodel/layout/component/LayoutSchema.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/box.h>
#include <gtkmm/window.h>

#include <cstdint>
#include <string>

namespace ao::gtk::layout::test
{
  using namespace uimodel;

  namespace
  {
    struct ActionRegistryFixture final
    {
      Glib::RefPtr<Gtk::Application> appPtr = ao::gtk::test::ensureGtkApplication();
      LayoutSchema schema;
      ActionRegistry registry{schema};
      Gtk::Window window{};
      Gtk::Box widget{};
      ActionActivationContext context{.parentWindow = window, .anchorWidget = widget, .componentId = "test_component"};
      ActionSchema actionSchema{.id = "test.action1", .label = "Test Action 1", .category = "Test", .capabilities = 0};
    };
  } // namespace

  TEST_CASE("ActionRegistry - registers and queries authoritative action schemas", "[gtk][unit][layout][action]")
  {
    auto fixture = ActionRegistryFixture{};
    auto& registry = fixture.registry;

    SECTION("Registers and retrieves actions")
    {
      std::int32_t calls = 0;
      REQUIRE(registry.tryRegisterAction(fixture.actionSchema, [&](auto&) { ++calls; }));

      auto const optActionSchema = registry.action("test.action1");
      REQUIRE(optActionSchema);
      CHECK(optActionSchema->id == "test.action1");
      CHECK(optActionSchema->label == "Test Action 1");
      CHECK(optActionSchema->category == "Test");
      CHECK(optActionSchema->capabilities == 0U);

      auto const all = registry.actions();
      REQUIRE(all.size() == 1);
      CHECK(all[0].id == "test.action1");
      CHECK(all[0].label == "Test Action 1");
      CHECK(all[0].category == "Test");
      CHECK(all[0].capabilities == 0U);
      CHECK(registry.tryActivate("test.action1", fixture.context));
      CHECK(calls == 1);
    }

    SECTION("Rejects duplicate ids")
    {
      std::int32_t originalCalls = 0;
      std::int32_t replacementCalls = 0;
      REQUIRE(registry.tryRegisterAction(fixture.actionSchema, [&](auto&) { ++originalCalls; }));
      auto replacementSchema = fixture.actionSchema;
      replacementSchema.label = "Replacement";
      REQUIRE_FALSE(registry.tryRegisterAction(replacementSchema, [&](auto&) { ++replacementCalls; }));

      auto const all = registry.actions();
      REQUIRE(all.size() == 1);
      CHECK(all[0].id == "test.action1");
      CHECK(all[0].label == "Test Action 1");
      CHECK(registry.tryActivate("test.action1", fixture.context));
      CHECK(originalCalls == 1);
      CHECK(replacementCalls == 0);
    }

    SECTION("Empty registry returns no schema entries and unknown id lookup returns nullopt")
    {
      CHECK(registry.actions().empty());
      CHECK_FALSE(registry.action("unknown"));
    }
  }

  TEST_CASE("ActionRegistry - dispatches with live context and availability", "[gtk][unit][layout][action]")
  {
    auto fixture = ActionRegistryFixture{};
    auto& registry = fixture.registry;

    SECTION("Activates handlers with context")
    {
      std::int32_t calls = 0;
      auto componentId = std::string{};
      Gtk::Window* parentWindow = nullptr;
      Gtk::Widget* anchorWidget = nullptr;
      REQUIRE(registry.tryRegisterAction(fixture.actionSchema,
                                         [&](ActionActivationContext const& ctx)
                                         {
                                           ++calls;
                                           componentId = ctx.componentId;
                                           parentWindow = &ctx.parentWindow;
                                           anchorWidget = &ctx.anchorWidget;
                                         }));

      CHECK(registry.tryActivate("test.action1", fixture.context));
      CHECK(calls == 1);
      CHECK(componentId == "test_component");
      CHECK(parentWindow == &fixture.window);
      CHECK(anchorWidget == &fixture.widget);
    }

    SECTION("Does not dispatch disabled actions")
    {
      bool enabled = false;
      std::int32_t calls = 0;
      REQUIRE(registry.tryRegisterAction(
        fixture.actionSchema,
        [&](auto&) { ++calls; },
        [&](auto const&) { return ActionAvailability{.enabled = enabled, .disabledReason = enabled ? "" : "Test"}; }));

      auto const disabled = registry.state("test.action1", fixture.context);
      CHECK_FALSE(disabled.enabled);
      CHECK(disabled.disabledReason == "Test");
      CHECK_FALSE(registry.tryActivate("test.action1", fixture.context));
      CHECK(calls == 0);

      enabled = true;
      auto const available = registry.state("test.action1", fixture.context);
      CHECK(available.enabled);
      CHECK(available.disabledReason.empty());
      CHECK(registry.tryActivate("test.action1", fixture.context));
      CHECK(calls == 1);
    }

    SECTION("Activating an unknown action id returns false")
    {
      CHECK_FALSE(registry.tryActivate("unknown", fixture.context));
    }

    SECTION("State provider is called during activate() to gate dispatch")
    {
      std::int32_t stateCalls = 0;
      std::int32_t handlerCalls = 0;
      auto componentId = std::string{};
      Gtk::Window* parentWindow = nullptr;
      Gtk::Widget* anchorWidget = nullptr;
      REQUIRE(registry.tryRegisterAction(
        fixture.actionSchema,
        [&](auto&) { ++handlerCalls; },
        [&](ActionActivationContext const& ctx)
        {
          ++stateCalls;
          componentId = ctx.componentId;
          parentWindow = &ctx.parentWindow;
          anchorWidget = &ctx.anchorWidget;
          return ActionAvailability{.enabled = true, .disabledReason = ""};
        }));

      CHECK(registry.tryActivate("test.action1", fixture.context));
      CHECK(stateCalls == 1);
      CHECK(handlerCalls == 1);
      CHECK(componentId == "test_component");
      CHECK(parentWindow == &fixture.window);
      CHECK(anchorWidget == &fixture.widget);
    }
  }
} // namespace ao::gtk::layout::test
