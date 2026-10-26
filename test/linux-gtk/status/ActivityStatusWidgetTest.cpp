// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "status/ActivityStatusWidget.h"

#include "test/MessageCatalogTestSupport.h"
#include "test/linux-gtk/GtkApplicationTestSupport.h"
#include "test/linux-gtk/GtkLayoutTestSupport.h"
#include "test/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/linux-gtk/GtkWidgetTestSupport.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/application.h>
#include <gtkmm/button.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/widget.h>

#include <cstddef>
#include <string>

namespace ao::gtk::test
{
  namespace
  {
    ActivityStatusWidget makeStatus(rt::AppRuntime& runtime, ActivityStatusWidgetOptions options = {})
    {
      return ActivityStatusWidget{ActivityStatusWidgetDependencies{
        .notifications = runtime.notifications(),
        .textCatalog = ao::test::englishMessageCatalog(),
        .libraryJobs = nullptr,
        .options = options,
      }};
    }

    struct MountedActivityStatus final
    {
      Glib::RefPtr<Gtk::Application> appPtr = ensureGtkApplication();
      GtkRuntimeFixture runtimeFixture;
      ActivityStatusWidget status;
      GtkWindowFixture windowFixture;

      explicit MountedActivityStatus(ActivityStatusWidgetOptions options = {})
        : status{makeStatus(runtimeFixture.runtime(), options)}
      {
        windowFixture.mount(status.widget());
        windowFixture.present();
      }

      rt::AppRuntime& runtime() { return runtimeFixture.runtime(); }
      void drain() { windowFixture.drain(); }
    };
  } // namespace

  TEST_CASE("ActivityStatusWidget - renders compact notification state", "[gtk][unit][status][activity]")
  {
    auto mounted = MountedActivityStatus{};
    auto& runtime = mounted.runtime();
    auto& status = mounted.status;

    SECTION("ambient idle is hidden")
    {
      CHECK_FALSE(status.widget().get_visible());
      CHECK(hasCssClass(status.widget(), "ao-activity-status-ambient"));
      CHECK(hasCssClass(status.widget(), "ao-activity-status-idle"));
      CHECK_FALSE(hasCssClass(status.widget(), "ao-activity-status-classic-inline"));
    }

    SECTION("warning notifications render as persistent readout")
    {
      runtime.notifications().post(
        rt::NotificationSeverity::Warning, "Partial import", rt::NotificationLifetime::pinned());
      mounted.drain();

      CHECK(status.widget().get_visible());
      CHECK(status.label().get_text() == "Partial import");
      CHECK(status.dismissButton().get_visible());
      CHECK(hasAccessibleLabel(status.dismissButton(), "Hide status"));
      CHECK(hasCssClass(status.widget(), "ao-activity-status-warning"));
    }
  }

  TEST_CASE("ActivityStatusWidget - separates compact dismissal from feed retention", "[gtk][unit][status][activity]")
  {
    auto mounted = MountedActivityStatus{};
    auto& runtime = mounted.runtime();
    auto& status = mounted.status;

    runtime.notifications().post(rt::NotificationSeverity::Error, "Scan failed", rt::NotificationLifetime::pinned());
    mounted.drain();
    REQUIRE(status.widget().get_visible());
    CHECK(hasCssClass(status.widget(), "ao-activity-status-error"));

    emitClicked(status.dismissButton());
    mounted.drain();

    CHECK_FALSE(status.widget().get_visible());
    CHECK(hasCssClass(status.widget(), "ao-activity-status-idle"));
    CHECK_FALSE(hasCssClass(status.widget(), "ao-activity-status-error"));
    CHECK(runtime.notifications().feed().entries.size() == 1);
    auto* const retainedMessage = findWidgetByClass<Gtk::Label>(status.detailContent(), "ao-activity-detail-title");
    REQUIRE(retainedMessage != nullptr);
    CHECK(retainedMessage->get_text() == "Scan failed");
  }

  TEST_CASE("ActivityStatusWidget - enables minimal detail popover only when detail exists",
            "[gtk][unit][status][activity]")
  {
    auto mounted = MountedActivityStatus{};
    auto& runtime = mounted.runtime();
    auto& status = mounted.status;

    SECTION("warning notification opens a compact detail row")
    {
      runtime.notifications().post(
        rt::NotificationSeverity::Warning, "Partial import", rt::NotificationLifetime::pinned());
      mounted.drain();

      CHECK(hasCssClass(status.widget(), "ao-activity-status-openable"));
      REQUIRE(status.detailButton().get_sensitive());
      auto* const message = findWidgetByClass<Gtk::Label>(status.detailContent(), "ao-activity-detail-title");
      REQUIRE(message != nullptr);
      CHECK(message->get_text() == "Partial import");
    }

    SECTION("plain info notification does not open detail")
    {
      runtime.notifications().post(
        rt::NotificationSeverity::Info, "Saved playlist", rt::NotificationLifetime::transient());
      mounted.drain();

      CHECK(status.widget().get_visible());
      CHECK(hasCssClass(status.widget(), "ao-activity-status-info"));
      CHECK_FALSE(hasCssClass(status.widget(), "ao-activity-status-openable"));
      CHECK_FALSE(status.detailButton().get_sensitive());
      mounted.drain();

      CHECK_FALSE(status.detailPopover().get_visible());
    }

    SECTION("open detail closes when compact status becomes hidden")
    {
      runtime.notifications().post(rt::NotificationRequest{
        .severity = rt::NotificationSeverity::Warning,
        .message = "Partial import",
        .lifetime = rt::NotificationLifetime::history(),
      });
      mounted.drain();
      REQUIRE(status.detailButton().get_sensitive());

      status.detailPopover().popup();
      mounted.drain();
      REQUIRE(status.detailPopover().get_visible());

      auto* const dismissButton = findWidgetByClass<Gtk::Button>(status.detailContent(), "ao-activity-detail-dismiss");
      REQUIRE(dismissButton != nullptr);
      CHECK(hasAccessibleLabel(*dismissButton, "Hide notification from status"));
      emitClicked(*dismissButton);
      mounted.drain();

      CHECK_FALSE(status.widget().get_visible());
      CHECK_FALSE(status.detailPopover().get_visible());
    }
  }

  TEST_CASE("ActivityStatusWidget - long notification details wrap without losing diagnostic text",
            "[gtk][unit][status][activity]")
  {
    auto mounted = MountedActivityStatus{};
    auto message = std::string{};

    SECTION("a complete CUE diagnosis wraps at a bounded natural width")
    {
      for (std::size_t index = 0; index < 12; ++index)
      {
        message += "The CUE layout is invalid or differs from imported ranges. Check the sheet and scan again. ";
      }
    }

    SECTION("an unbroken UTF-8 URI can wrap between characters")
    {
      for (std::size_t index = 0; index < 120; ++index)
      {
        message += "音乐";
      }

      message += std::string(600, 'x') + ".flac";
    }

    mounted.runtime().notifications().post(
      rt::NotificationSeverity::Warning, message, rt::NotificationLifetime::history());
    mounted.drain();
    auto* const title = findWidgetByClass<Gtk::Label>(mounted.status.detailContent(), "ao-activity-detail-title");
    REQUIRE(title != nullptr);
    CHECK(title->get_text() == message);

    auto unwrapped = Gtk::Label{message};
    auto const width = measureWidget(*title, Gtk::Orientation::HORIZONTAL);
    auto const unwrappedWidth = measureWidget(unwrapped, Gtk::Orientation::HORIZONTAL);
    REQUIRE(width.natural > 0);
    CHECK(width.minimum <= width.natural);
    CHECK(width.natural < unwrappedWidth.natural);
    CHECK(width.minimum < unwrappedWidth.minimum);

    auto const height = measureWidget(*title, Gtk::Orientation::VERTICAL, width.natural);
    auto const unwrappedHeight = measureWidget(unwrapped, Gtk::Orientation::VERTICAL, unwrappedWidth.natural);
    CHECK(height.natural > unwrappedHeight.natural);
  }

  TEST_CASE("ActivityStatusWidget - detail dismiss hides row without deleting notification feed",
            "[gtk][unit][status][activity]")
  {
    auto mounted = MountedActivityStatus{};
    auto& runtime = mounted.runtime();
    auto& status = mounted.status;

    runtime.notifications().post(
      rt::NotificationSeverity::Warning, "Partial import", rt::NotificationLifetime::history());
    mounted.drain();

    auto* const dismissButton = findWidgetByClass<Gtk::Button>(status.detailContent(), "ao-activity-detail-dismiss");
    REQUIRE(dismissButton != nullptr);

    emitClicked(*dismissButton);
    mounted.drain();

    CHECK(runtime.notifications().feed().entries.size() == 1);
    CHECK(findWidgetByClass<Gtk::Label>(status.detailContent(), "ao-activity-detail-title") == nullptr);
    CHECK_FALSE(status.widget().get_visible());
    CHECK_FALSE(status.detailButton().get_sensitive());
  }

  TEST_CASE("ActivityStatusWidget - classic inline reserves idle space", "[gtk][unit][status][activity]")
  {
    auto options = ActivityStatusWidgetOptions{.variant = ActivityStatusWidgetVariant::ClassicInline,
                                               .idleBehavior = ActivityStatusWidgetIdleBehavior::Reserve,
                                               .maxTextChars = 42};
    auto mounted = MountedActivityStatus{options};
    auto& status = mounted.status;

    CHECK(status.widget().get_visible());
    CHECK(status.label().get_text().empty());
    CHECK(hasCssClass(status.widget(), "ao-activity-status-classic-inline"));
    CHECK_FALSE(hasCssClass(status.widget(), "ao-activity-status-ambient"));
  }
} // namespace ao::gtk::test
