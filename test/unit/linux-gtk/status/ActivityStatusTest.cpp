// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "status/ActivityStatus.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/application.h>
#include <gtkmm/button.h>
#include <gtkmm/progressbar.h>
#include <gtkmm/widget.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::gtk::test
{
  class ActivityStatusTestPeer final
  {
  public:
    static void renderLibraryTaskProgress(ActivityStatus& status, std::string message, double const fraction)
    {
      status._model.onLibraryTaskProgress(std::move(message), fraction);
      status.render();
    }

    static void renderLibraryTaskCompleted(ActivityStatus& status, std::size_t const count)
    {
      status._model.onLibraryTaskCompleted(count, status._notifications.feed());
      status.render();
    }
  };

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

    struct MountedActivityStatus final
    {
      Glib::RefPtr<Gtk::Application> appPtr = ensureGtkApplication();
      GtkRuntimeFixture runtimeFixture;
      ActivityStatus status;
      GtkWindowFixture windowFixture;

      explicit MountedActivityStatus(ActivityStatusOptions options = {},
                                     ActivityStatusActionResolver resolveNotificationAction = {},
                                     ActivityStatusActionHandler onNotificationAction = {})
        : status{makeStatus(runtimeFixture.runtime(),
                            options,
                            std::move(resolveNotificationAction),
                            std::move(onNotificationAction))}
      {
        windowFixture.mount(status.widget());
        windowFixture.present();
      }

      rt::AppRuntime& runtime() { return runtimeFixture.runtime(); }
      void drain() { windowFixture.drain(); }
    };
  } // namespace

  TEST_CASE("ActivityStatus - renders compact notification state", "[gtk][unit][status][activity]")
  {
    auto mounted = MountedActivityStatus{};
    auto& runtime = mounted.runtime();
    auto& status = mounted.status;

    SECTION("ambient idle is hidden")
    {
      CHECK_FALSE(status.widget().get_visible());
      CHECK(hasCssClass(status.widget(), "ao-activity-status-ambient"));
      CHECK_FALSE(hasCssClass(status.widget(), "ao-activity-status-classic-inline"));
    }

    SECTION("warning notifications render as persistent readout")
    {
      runtime.notifications().post(rt::NotificationSeverity::Warning, "Partial import", true);
      mounted.drain();

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
      mounted.drain();

      CHECK_FALSE(status.widget().get_visible());
      CHECK(findWidgetByClass<Gtk::Label>(status.detailContentForTest(), "ao-activity-detail-title") == nullptr);
    }
  }

  TEST_CASE("ActivityStatus - separates compact dismissal from feed retention", "[gtk][unit][status][activity]")
  {
    auto mounted = MountedActivityStatus{};
    auto& runtime = mounted.runtime();
    auto& status = mounted.status;

    runtime.notifications().post(rt::NotificationSeverity::Error, "Scan failed", true);
    mounted.drain();
    REQUIRE(status.widget().get_visible());

    emitClicked(status.dismissButtonForTest());
    mounted.drain();

    CHECK_FALSE(status.widget().get_visible());
    CHECK(runtime.notifications().feed().entries.size() == 1);
    auto* const retainedTitle =
      findWidgetByClass<Gtk::Label>(status.detailContentForTest(), "ao-activity-detail-title");
    REQUIRE(retainedTitle != nullptr);
    CHECK(retainedTitle->get_text() == "Scan failed");
  }

  TEST_CASE("ActivityStatus - enables minimal detail popover only when detail exists", "[gtk][unit][status][activity]")
  {
    auto mounted = MountedActivityStatus{};
    auto& runtime = mounted.runtime();
    auto& status = mounted.status;

    SECTION("warning notification opens a compact detail row")
    {
      runtime.notifications().post(rt::NotificationSeverity::Warning, "Partial import", true);
      mounted.drain();

      CHECK(hasCssClass(status.widget(), "ao-activity-status-openable"));
      REQUIRE(status.detailButtonForTest().get_sensitive());
      auto* const title = findWidgetByClass<Gtk::Label>(status.detailContentForTest(), "ao-activity-detail-title");
      REQUIRE(title != nullptr);
      CHECK(title->get_text() == "Partial import");
    }

    SECTION("plain info notification does not open detail")
    {
      runtime.notifications().post(rt::NotificationSeverity::Info, "Saved playlist");
      mounted.drain();

      CHECK(status.widget().get_visible());
      CHECK_FALSE(hasCssClass(status.widget(), "ao-activity-status-openable"));
      CHECK_FALSE(status.detailButtonForTest().get_sensitive());
      mounted.drain();

      CHECK_FALSE(status.detailPopoverForTest().get_visible());
    }

    SECTION("detail-only notification has no hidden idle compact affordance")
    {
      runtime.notifications().post(rt::NotificationRequest{
        .severity = rt::NotificationSeverity::Info,
        .message = "Index diagnostic",
        .activityPresentation = rt::NotificationActivityPresentation::DetailOnly,
      });
      mounted.drain();

      CHECK_FALSE(status.widget().get_visible());
      CHECK_FALSE(status.detailButtonForTest().get_sensitive());
      CHECK_FALSE(hasCssClass(status.widget(), "ao-activity-status-openable"));
      auto* const title = findWidgetByClass<Gtk::Label>(status.detailContentForTest(), "ao-activity-detail-title");
      REQUIRE(title != nullptr);
      CHECK(title->get_text() == "Index diagnostic");
    }

    SECTION("open detail closes when compact status becomes hidden")
    {
      runtime.notifications().post(rt::NotificationRequest{
        .severity = rt::NotificationSeverity::Warning,
        .message = "Partial import",
        .content = rt::NotificationContentState{.title = "Import"},
      });
      mounted.drain();
      REQUIRE(status.detailButtonForTest().get_sensitive());

      status.detailPopoverForTest().popup();
      mounted.drain();
      REQUIRE(status.detailPopoverForTest().get_visible());

      auto* const dismissButton =
        findWidgetByClass<Gtk::Button>(status.detailContentForTest(), "ao-activity-detail-dismiss");
      REQUIRE(dismissButton != nullptr);
      emitClicked(*dismissButton);
      mounted.drain();

      CHECK_FALSE(status.widget().get_visible());
      CHECK_FALSE(status.detailPopoverForTest().get_visible());
    }
  }

  TEST_CASE("ActivityStatus - detail dismiss hides row without deleting notification feed",
            "[gtk][unit][status][activity]")
  {
    auto mounted = MountedActivityStatus{};
    auto& runtime = mounted.runtime();
    auto& status = mounted.status;

    runtime.notifications().post(rt::NotificationSeverity::Warning, "Partial import");
    mounted.drain();

    auto* const dismissButton =
      findWidgetByClass<Gtk::Button>(status.detailContentForTest(), "ao-activity-detail-dismiss");
    REQUIRE(dismissButton != nullptr);

    emitClicked(*dismissButton);
    mounted.drain();

    CHECK(runtime.notifications().feed().entries.size() == 1);
    CHECK(findWidgetByClass<Gtk::Label>(status.detailContentForTest(), "ao-activity-detail-title") == nullptr);
    CHECK_FALSE(status.widget().get_visible());
    CHECK_FALSE(status.detailButtonForTest().get_sensitive());
  }

  TEST_CASE("ActivityStatus - detail actions require explicit handler", "[gtk][unit][status][activity]")
  {
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
      auto activations = std::vector<std::pair<rt::NotificationId, std::string>>{};
      Gtk::Widget* activationAnchor = nullptr;
      auto mounted =
        MountedActivityStatus{{},
                              allActionsEnabled(),
                              [&activations, &activationAnchor](
                                rt::NotificationId const id, std::string_view const actionId, Gtk::Widget& anchor)
                              {
                                activations.emplace_back(id, std::string{actionId});
                                activationAnchor = &anchor;
                              }};
      auto& runtime = mounted.runtime();
      auto& status = mounted.status;

      auto const id = runtime.notifications().post(request);
      mounted.drain();

      auto buttons = actionButtons(status.detailContentForTest());
      REQUIRE(buttons.size() == 2);
      CHECK(buttons[0]->get_label() == "Retry");
      CHECK(buttons[1]->get_label() == "Ignore");

      emitClicked(*buttons[0]);
      mounted.drain();

      REQUIRE(activations.size() == 1);
      CHECK(activations[0].first == id);
      CHECK(activations[0].second == "library.retry");
      CHECK(activationAnchor == buttons[0]);
      CHECK(runtime.notifications().feed().entries.size() == 1);
    }

    SECTION("actions are not shown without a handler")
    {
      auto mounted = MountedActivityStatus{};
      auto& runtime = mounted.runtime();
      auto& status = mounted.status;

      runtime.notifications().post(request);
      mounted.drain();

      CHECK(actionButtons(status.detailContentForTest()).empty());
    }

    SECTION("actions are not shown without a resolver")
    {
      auto mounted = MountedActivityStatus{{}, {}, [](rt::NotificationId, std::string_view, Gtk::Widget&) {}};
      auto& runtime = mounted.runtime();
      auto& status = mounted.status;

      runtime.notifications().post(request);
      mounted.drain();

      CHECK(actionButtons(status.detailContentForTest()).empty());
    }

    SECTION("resolver hides unknown actions and disables unavailable actions")
    {
      auto mounted = MountedActivityStatus{
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
        [](rt::NotificationId, std::string_view, Gtk::Widget&) {}};
      auto& runtime = mounted.runtime();
      auto& status = mounted.status;

      runtime.notifications().post(request);
      mounted.drain();

      auto buttons = actionButtons(status.detailContentForTest());
      REQUIRE(buttons.size() == 1);
      CHECK(buttons[0]->get_label() == "Retry");
      CHECK_FALSE(buttons[0]->get_sensitive());
      CHECK(buttons[0]->get_tooltip_text() == "Library busy");
    }

    SECTION("resolver label fallback avoids empty action buttons")
    {
      auto mounted = MountedActivityStatus{
        {},
        [](std::string_view actionId, std::string_view label)
        {
          auto resolvedLabel = label.empty() && actionId == "library.retry" ? std::string{"Retry"} : std::string{label};
          return ActivityStatusActionRenderState{
            .visible = !resolvedLabel.empty(), .enabled = true, .label = std::move(resolvedLabel)};
        },
        [](rt::NotificationId, std::string_view, Gtk::Widget&) {}};
      auto& runtime = mounted.runtime();
      auto& status = mounted.status;

      runtime.notifications().post(rt::NotificationRequest{
        .severity = rt::NotificationSeverity::Warning,
        .message = "Partial import",
        .content =
          rt::NotificationContentState{
            .title = "Import",
            .actions = {{.id = "library.retry", .label = ""}, {.id = "library.ignore", .label = ""}},
          },
      });
      mounted.drain();

      auto buttons = actionButtons(status.detailContentForTest());
      REQUIRE(buttons.size() == 1);
      CHECK(buttons[0]->get_label() == "Retry");
    }
  }

  TEST_CASE("ActivityStatus - classic inline reserves idle space", "[gtk][unit][status][activity]")
  {
    auto options = ActivityStatusOptions{.variant = ActivityStatusVariant::ClassicInline,
                                         .idleBehavior = ActivityStatusIdleBehavior::Reserve,
                                         .maxTextChars = 42};
    auto mounted = MountedActivityStatus{options};
    auto& status = mounted.status;

    CHECK(status.widget().get_visible());
    CHECK(status.labelForTest().get_text().empty());
    CHECK(hasCssClass(status.widget(), "ao-activity-status-classic-inline"));
    CHECK_FALSE(hasCssClass(status.widget(), "ao-activity-status-ambient"));
  }

  TEST_CASE("ActivityStatus - renders library task progress binding", "[gtk][unit][status][activity]")
  {
    auto mounted = MountedActivityStatus{};
    auto& status = mounted.status;

    ActivityStatusTestPeer::renderLibraryTaskProgress(status, "Updating: status-progress.flac", 0.625);

    CHECK(status.widget().get_visible());
    CHECK(status.labelForTest().get_text() == "Updating library");
    CHECK(status.progressForTest().get_visible());
    CHECK(status.progressForTest().get_fraction() == 0.625);
    CHECK(hasCssClass(status.widget(), "ao-activity-status-processing"));
    CHECK(status.detailButtonForTest().get_sensitive());

    auto* const taskMessage =
      findWidgetByClass<Gtk::Label>(status.detailContentForTest(), "ao-activity-detail-message");
    REQUIRE(taskMessage != nullptr);
    CHECK(taskMessage->get_text() == "Updating: status-progress.flac");
    auto* const taskProgress =
      findWidgetByClass<Gtk::ProgressBar>(status.detailContentForTest(), "ao-activity-detail-progress");
    REQUIRE(taskProgress != nullptr);
    CHECK(taskProgress->get_fraction() == 0.625);

    ActivityStatusTestPeer::renderLibraryTaskCompleted(status, 4);

    CHECK(status.widget().get_visible());
    CHECK(status.labelForTest().get_text() == "Scan complete: 4 tracks added");
    CHECK_FALSE(status.progressForTest().get_visible());
    CHECK(hasCssClass(status.widget(), "ao-activity-status-success"));
  }
} // namespace ao::gtk::test
