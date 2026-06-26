// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "status/ActivityStatus.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/StateTypes.h>
#include <ao/uimodel/status/ActivityStatusModel.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/button.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::gtk::test
{
  namespace
  {
    ActivityStatus makeStatus(rt::AppRuntime& runtime,
                              ActivityStatusOptions options = {},
                              ActivityStatusActionResolver resolveNotificationAction = {},
                              ActivityStatusActionHandler onNotificationAction = {})
    {
      return ActivityStatus{ActivityStatusDependencies{
        .notifications = runtime.notifications(),
        .libraryChanges = nullptr,
        .options = options,
        .resolveNotificationAction = std::move(resolveNotificationAction),
        .onNotificationAction = std::move(onNotificationAction),
      }};
    }

    ActivityStatusActionResolver allActionsEnabled()
    {
      return [](std::string_view, std::string_view label)
      {
        return ActivityStatusActionRenderState{.visible = !label.empty(), .enabled = true, .label = std::string{label}};
      };
    }

    std::vector<Gtk::Button*> actionButtons(Gtk::Widget& root)
    {
      auto result = std::vector<Gtk::Button*>{};

      for (auto* const button : collectAll<Gtk::Button>(root))
      {
        if (hasCssClass(*button, "ao-activity-detail-action"))
        {
          result.push_back(button);
        }
      }

      return result;
    }
  } // namespace

  TEST_CASE("ActivityStatus - renders compact notification state", "[gtk][status][activity]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto window = Gtk::Window{};
    auto status = makeStatus(runtime);
    window.set_child(status.widget());
    window.present();
    drainGtkEvents();

    SECTION("ambient idle is hidden")
    {
      CHECK_FALSE(status.widget().get_visible());
      CHECK(status.viewStateForTest().compact.kind == uimodel::status::ActivityStatusKind::Idle);
      CHECK(hasCssClass(status.widget(), "ao-activity-status-ambient"));
      CHECK_FALSE(hasCssClass(status.widget(), "ao-activity-status-classic-inline"));
    }

    SECTION("warning notifications render as persistent readout")
    {
      runtime.notifications().post(rt::NotificationSeverity::Warning, "Partial import", true);
      drainGtkEvents();

      CHECK(status.widget().get_visible());
      CHECK(status.labelForTest().get_text() == "Partial import");
      CHECK(status.dismissButtonForTest().get_visible());
      CHECK(hasCssClass(status.widget(), "ao-activity-status-warning"));
    }

    SECTION("startup ready notification does not surface compact")
    {
      runtime.notifications().post(rt::NotificationRequest{
        .severity = rt::NotificationSeverity::Info,
        .message = "Aobus Ready",
        .activityPresentation = rt::NotificationActivityPresentation::Hidden,
      });
      drainGtkEvents();

      CHECK_FALSE(status.widget().get_visible());
      CHECK(status.viewStateForTest().detail.items.empty());
    }

    window.unset_child();
  }

  TEST_CASE("ActivityStatus - separates compact dismissal from feed retention", "[gtk][status][activity]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto window = Gtk::Window{};
    auto status = makeStatus(runtime);
    window.set_child(status.widget());
    window.present();
    drainGtkEvents();

    runtime.notifications().post(rt::NotificationSeverity::Error, "Scan failed", true);
    drainGtkEvents();
    REQUIRE(status.widget().get_visible());

    emitClicked(status.dismissButtonForTest());
    drainGtkEvents();

    CHECK_FALSE(status.widget().get_visible());
    CHECK(runtime.notifications().feed().entries.size() == 1);
    CHECK(status.viewStateForTest().detail.items.size() == 1);

    window.unset_child();
  }

  TEST_CASE("ActivityStatus - enables minimal detail popover only when detail exists", "[gtk][status][activity]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto window = Gtk::Window{};
    auto status = makeStatus(runtime);
    window.set_child(status.widget());
    window.present();
    drainGtkEvents();

    SECTION("warning notification opens a compact detail row")
    {
      runtime.notifications().post(rt::NotificationSeverity::Warning, "Partial import", true);
      drainGtkEvents();

      CHECK(hasCssClass(status.widget(), "ao-activity-status-openable"));
      REQUIRE(status.detailButtonForTest().get_sensitive());
      auto* const title = findWidgetByClass<Gtk::Label>(status.detailContentForTest(), "ao-activity-detail-title");
      REQUIRE(title != nullptr);
      CHECK(title->get_text() == "Partial import");
    }

    SECTION("plain info notification does not open detail")
    {
      runtime.notifications().post(rt::NotificationSeverity::Info, "Saved playlist");
      drainGtkEvents();

      CHECK(status.widget().get_visible());
      CHECK_FALSE(hasCssClass(status.widget(), "ao-activity-status-openable"));
      CHECK_FALSE(status.detailButtonForTest().get_sensitive());
      drainGtkEvents();

      CHECK_FALSE(status.detailPopoverForTest().get_visible());
    }

    SECTION("detail-only notification has no hidden idle compact affordance")
    {
      runtime.notifications().post(rt::NotificationRequest{
        .severity = rt::NotificationSeverity::Info,
        .message = "Index diagnostic",
        .activityPresentation = rt::NotificationActivityPresentation::DetailOnly,
      });
      drainGtkEvents();

      CHECK_FALSE(status.widget().get_visible());
      CHECK_FALSE(status.detailButtonForTest().get_sensitive());
      CHECK_FALSE(hasCssClass(status.widget(), "ao-activity-status-openable"));
      REQUIRE(status.viewStateForTest().detail.items.size() == 1);
    }

    SECTION("open detail closes when compact status becomes hidden")
    {
      runtime.notifications().post(rt::NotificationRequest{
        .severity = rt::NotificationSeverity::Warning,
        .message = "Partial import",
        .content = rt::NotificationContentState{.title = "Import"},
      });
      drainGtkEvents();
      REQUIRE(status.detailButtonForTest().get_sensitive());

      status.detailPopoverForTest().popup();
      drainGtkEvents();
      REQUIRE(status.detailPopoverForTest().get_visible());

      auto* const dismissButton =
        findWidgetByClass<Gtk::Button>(status.detailContentForTest(), "ao-activity-detail-dismiss");
      REQUIRE(dismissButton != nullptr);
      emitClicked(*dismissButton);
      drainGtkEvents();

      CHECK_FALSE(status.widget().get_visible());
      CHECK_FALSE(status.detailPopoverForTest().get_visible());
    }

    window.unset_child();
  }

  TEST_CASE("ActivityStatus - detail dismiss hides row without deleting notification feed", "[gtk][status][activity]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto window = Gtk::Window{};
    auto status = makeStatus(runtime);
    window.set_child(status.widget());
    window.present();
    drainGtkEvents();

    runtime.notifications().post(rt::NotificationSeverity::Warning, "Partial import");
    drainGtkEvents();

    auto* const dismissButton =
      findWidgetByClass<Gtk::Button>(status.detailContentForTest(), "ao-activity-detail-dismiss");
    REQUIRE(dismissButton != nullptr);

    emitClicked(*dismissButton);
    drainGtkEvents();

    CHECK(runtime.notifications().feed().entries.size() == 1);
    CHECK(status.viewStateForTest().detail.items.empty());
    CHECK_FALSE(status.widget().get_visible());
    CHECK_FALSE(status.detailButtonForTest().get_sensitive());

    window.unset_child();
  }

  TEST_CASE("ActivityStatus - detail actions require explicit handler", "[gtk][status][activity]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto request = rt::NotificationRequest{
      .severity = rt::NotificationSeverity::Warning,
      .message = "Partial import",
      .content =
        rt::NotificationContentState{
          .title = "Import",
          .actions = {{.id = "library.retry", .label = "Retry"},
                      {.id = "library.ignore", .label = "Ignore"},
                      {.id = "library.extra", .label = "Extra"}},
        },
    };

    SECTION("handler receives notification and action ids")
    {
      auto fixture = GtkRuntimeFixture{};
      auto& runtime = fixture.runtime();
      auto activations = std::vector<std::pair<rt::NotificationId, std::string>>{};
      Gtk::Widget* activationAnchor = nullptr;
      auto window = Gtk::Window{};
      auto status = makeStatus(runtime,
                               {},
                               allActionsEnabled(),
                               [&activations, &activationAnchor](
                                 rt::NotificationId const id, std::string_view const actionId, Gtk::Widget& anchor)
                               {
                                 activations.emplace_back(id, std::string{actionId});
                                 activationAnchor = &anchor;
                               });
      window.set_child(status.widget());
      window.present();
      drainGtkEvents();

      auto const id = runtime.notifications().post(request);
      drainGtkEvents();

      auto buttons = actionButtons(status.detailContentForTest());
      REQUIRE(buttons.size() == 2);
      CHECK(buttons[0]->get_label() == "Retry");
      CHECK(buttons[1]->get_label() == "Ignore");

      emitClicked(*buttons[0]);
      drainGtkEvents();

      REQUIRE(activations.size() == 1);
      CHECK(activations[0].first == id);
      CHECK(activations[0].second == "library.retry");
      CHECK(activationAnchor == buttons[0]);
      CHECK(runtime.notifications().feed().entries.size() == 1);

      window.unset_child();
    }

    SECTION("actions are not shown without a handler")
    {
      auto fixture = GtkRuntimeFixture{};
      auto& runtime = fixture.runtime();
      auto window = Gtk::Window{};
      auto status = makeStatus(runtime);
      window.set_child(status.widget());
      window.present();
      drainGtkEvents();

      runtime.notifications().post(request);
      drainGtkEvents();

      CHECK(actionButtons(status.detailContentForTest()).empty());

      window.unset_child();
    }

    SECTION("actions are not shown without a resolver")
    {
      auto fixture = GtkRuntimeFixture{};
      auto& runtime = fixture.runtime();
      auto window = Gtk::Window{};
      auto status = makeStatus(runtime, {}, {}, [](rt::NotificationId, std::string_view, Gtk::Widget&) {});
      window.set_child(status.widget());
      window.present();
      drainGtkEvents();

      runtime.notifications().post(request);
      drainGtkEvents();

      CHECK(actionButtons(status.detailContentForTest()).empty());

      window.unset_child();
    }

    SECTION("resolver hides unknown actions and disables unavailable actions")
    {
      auto fixture = GtkRuntimeFixture{};
      auto& runtime = fixture.runtime();
      auto window = Gtk::Window{};
      auto status = makeStatus(
        runtime,
        {},
        [](std::string_view actionId, std::string_view label)
        {
          if (actionId == "library.retry")
          {
            return ActivityStatusActionRenderState{
              .visible = true, .enabled = false, .label = std::string{label}, .disabledReason = "Library busy"};
          }

          return ActivityStatusActionRenderState{};
        },
        [](rt::NotificationId, std::string_view, Gtk::Widget&) {});
      window.set_child(status.widget());
      window.present();
      drainGtkEvents();

      runtime.notifications().post(request);
      drainGtkEvents();

      auto buttons = actionButtons(status.detailContentForTest());
      REQUIRE(buttons.size() == 1);
      CHECK(buttons[0]->get_label() == "Retry");
      CHECK_FALSE(buttons[0]->get_sensitive());
      CHECK(buttons[0]->get_tooltip_text() == "Library busy");

      window.unset_child();
    }

    SECTION("resolver label fallback avoids empty action buttons")
    {
      auto fixture = GtkRuntimeFixture{};
      auto& runtime = fixture.runtime();
      auto window = Gtk::Window{};
      auto status = makeStatus(
        runtime,
        {},
        [](std::string_view actionId, std::string_view label)
        {
          auto resolvedLabel = label.empty() && actionId == "library.retry" ? std::string{"Retry"} : std::string{label};
          return ActivityStatusActionRenderState{
            .visible = !resolvedLabel.empty(), .enabled = true, .label = std::move(resolvedLabel)};
        },
        [](rt::NotificationId, std::string_view, Gtk::Widget&) {});
      window.set_child(status.widget());
      window.present();
      drainGtkEvents();

      runtime.notifications().post(rt::NotificationRequest{
        .severity = rt::NotificationSeverity::Warning,
        .message = "Partial import",
        .content =
          rt::NotificationContentState{
            .title = "Import",
            .actions = {{.id = "library.retry", .label = ""}, {.id = "library.ignore", .label = ""}},
          },
      });
      drainGtkEvents();

      auto buttons = actionButtons(status.detailContentForTest());
      REQUIRE(buttons.size() == 1);
      CHECK(buttons[0]->get_label() == "Retry");

      window.unset_child();
    }
  }

  TEST_CASE("ActivityStatus - classic inline reserves idle space", "[gtk][status][activity]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto options = ActivityStatusOptions{.variant = ActivityStatusVariant::ClassicInline,
                                         .idleBehavior = ActivityStatusIdleBehavior::Reserve,
                                         .maxTextChars = 42};
    auto status = makeStatus(runtime, options);

    CHECK(status.widget().get_visible());
    CHECK(status.labelForTest().get_text().empty());
    CHECK(hasCssClass(status.widget(), "ao-activity-status-classic-inline"));
    CHECK_FALSE(hasCssClass(status.widget(), "ao-activity-status-ambient"));
  }
} // namespace ao::gtk::test
