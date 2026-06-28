// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/linux-gtk/layout/component/status/StatusRegistry.h"
#include "app/linux-gtk/layout/runtime/ActionRegistry.h"
#include "app/linux-gtk/layout/runtime/ComponentRegistry.h"
#include "app/linux-gtk/layout/runtime/LayoutContext.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/rt/NotificationService.h>
#include <ao/rt/StateTypes.h>
#include <ao/uimodel/layout/action/LayoutActionTypes.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/application.h>
#include <gtkmm/button.h>
#include <gtkmm/window.h>

namespace ao::gtk::layout::test
{
  using namespace uimodel;
  using ao::gtk::test::drainGtkEvents;
  using ao::gtk::test::emitClicked;
  using ao::gtk::test::findWidgetByClass;

  TEST_CASE("Status bar components register status descriptors", "[gtk][unit][layout][status]")
  {
    auto const appPtr = Gtk::Application::create("io.github.aobus.status_test");

    auto registry = ComponentRegistry{};
    registerStatusComponents(registry);

    auto const optDesc = registry.descriptor("status.activityStatus");
    REQUIRE(optDesc.has_value());
    CHECK(optDesc->displayName == "Activity Status");

    auto const optSelectionDesc = registry.descriptor("status.selectionInfo");
    REQUIRE(optSelectionDesc.has_value());
    CHECK(optSelectionDesc->displayName == "Selection Info");

    CHECK_FALSE(registry.descriptor("status.statusSlot").has_value());
    CHECK_FALSE(registry.descriptor("status.notificationCenter").has_value());
  }

  TEST_CASE("status.activityStatus routes notification actions through ActionRegistry", "[gtk][unit][status]")
  {
    [[maybe_unused]] auto const appPtr = ao::gtk::test::ensureGtkApplication();
    auto fixture = ao::gtk::test::GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto window = Gtk::Window{};
    auto registry = ComponentRegistry{};
    registerStatusComponents(registry);

    auto actionRegistry = ActionRegistry{};
    bool activated = false;
    bool sawComponentId = false;
    bool sawActionAnchor = false;
    actionRegistry.registerAction(LayoutActionDescriptor{.id = "library.retry",
                                                         .label = "Retry Import",
                                                         .category = "Library",
                                                         .capabilities = LayoutActionCapability::None},
                                  [&](ActionActivationContext& ctx)
                                  {
                                    activated = true;
                                    sawComponentId = ctx.componentId == "activity-slot";
                                    sawActionAnchor = ctx.anchorWidget.has_css_class("ao-activity-detail-action");
                                  });

    auto ctx =
      LayoutContext{.registry = registry, .actionRegistry = actionRegistry, .runtime = runtime, .parentWindow = window};
    auto const node = LayoutNode{.id = "activity-slot", .type = "status.activityStatus"};
    auto const compPtr = registry.create(ctx, node);
    REQUIRE(compPtr != nullptr);
    window.set_child(compPtr->widget());
    window.present();
    drainGtkEvents();

    runtime.notifications().post(rt::NotificationRequest{
      .severity = rt::NotificationSeverity::Warning,
      .message = "Partial import",
      .content =
        rt::NotificationContentState{
          .title = "Import",
          .actions = {{.id = "library.retry", .label = "Retry"}},
        },
    });
    drainGtkEvents();

    auto* const actionButton = findWidgetByClass<Gtk::Button>(compPtr->widget(), "ao-activity-detail-action");
    REQUIRE(actionButton != nullptr);

    emitClicked(*actionButton);
    drainGtkEvents();

    CHECK(activated);
    CHECK(sawComponentId);
    CHECK(sawActionAnchor);
    CHECK(runtime.notifications().feed().entries.size() == 1);

    window.unset_child();
  }

  TEST_CASE("status.activityStatus validates notification actions before rendering", "[gtk][unit][status]")
  {
    [[maybe_unused]] auto const appPtr = ao::gtk::test::ensureGtkApplication();
    auto fixture = ao::gtk::test::GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto window = Gtk::Window{};
    auto registry = ComponentRegistry{};
    registerStatusComponents(registry);

    auto actionRegistry = ActionRegistry{};
    bool sawFallbackComponentId = false;
    actionRegistry.registerAction(
      LayoutActionDescriptor{.id = "library.retry",
                             .label = "Retry Import",
                             .category = "Library",
                             .capabilities = LayoutActionCapability::None},
      [](ActionActivationContext&) {},
      [&sawFallbackComponentId](ActionActivationContext const& stateCtx)
      {
        sawFallbackComponentId = stateCtx.componentId == "status.activityStatus";
        return LayoutActionState{.enabled = false, .disabledReason = "Library busy"};
      });

    auto ctx =
      LayoutContext{.registry = registry, .actionRegistry = actionRegistry, .runtime = runtime, .parentWindow = window};
    auto const node = LayoutNode{.id = "", .type = "status.activityStatus"};
    auto const compPtr = registry.create(ctx, node);
    REQUIRE(compPtr != nullptr);
    window.set_child(compPtr->widget());
    window.present();
    drainGtkEvents();

    runtime.notifications().post(rt::NotificationRequest{
      .severity = rt::NotificationSeverity::Warning,
      .message = "Partial import",
      .content =
        rt::NotificationContentState{
          .title = "Import",
          .actions = {{.id = "library.retry", .label = ""}, {.id = "library.unknown", .label = "Unknown"}},
        },
    });
    drainGtkEvents();

    auto* const actionButton = findWidgetByClass<Gtk::Button>(compPtr->widget(), "ao-activity-detail-action");
    REQUIRE(actionButton != nullptr);
    CHECK(actionButton->get_label() == "Retry Import");
    CHECK_FALSE(actionButton->get_sensitive());
    CHECK(actionButton->get_tooltip_text() == "Library busy");
    CHECK(sawFallbackComponentId);

    window.unset_child();
  }
} // namespace ao::gtk::layout::test
