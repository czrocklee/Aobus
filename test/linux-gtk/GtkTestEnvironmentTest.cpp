// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "GtkSessionBusTestSupport.h"
#include "test/fatal/ProbeProcess.h"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <gtk/gtk.h>

#include <chrono>
#include <string>

namespace ao::gtk::test
{
  TEST_CASE("GtkTestEnvironment - bus admission requires a matching owned Unix endpoint")
  {
    CHECK_FALSE(isOwnedGtkSessionBus(nullptr, nullptr));
    CHECK_FALSE(isOwnedGtkSessionBus("unix:path=/private/bus", nullptr));
    CHECK_FALSE(isOwnedGtkSessionBus(nullptr, "unix:path=/private/bus"));
    CHECK_FALSE(isOwnedGtkSessionBus("", ""));
    CHECK_FALSE(isOwnedGtkSessionBus("unix:", "unix:"));
    CHECK_FALSE(isOwnedGtkSessionBus("disabled:", "disabled:"));
    CHECK_FALSE(isOwnedGtkSessionBus("unix:path=/other/bus", "unix:path=/private/bus"));
    CHECK(isOwnedGtkSessionBus("unix:path=/private/bus,guid=abc", "unix:path=/private/bus,guid=abc"));
  }

  TEST_CASE("GtkTestEnvironment - direct entry disables unowned buses before test dispatch")
  {
    auto const executable = ao::test::currentProbeExecutablePath();
    REQUIRE_FALSE(executable.empty());

    for (auto const* const scenario : {"unowned-bus", "mismatched-bus", "owned-bus"})
    {
      INFO(scenario);
      auto const result = ao::test::runProbeProcess(executable, scenario, std::chrono::seconds{10});
      INFO(result.standardError);
      REQUIRE(result.hasSuccessfulExit());
      CHECK(result.standardOutput == std::string{scenario} + ": isolated=yes\n");
      CHECK(result.standardError.empty());
    }
  }

  TEST_CASE("GtkTestEnvironment - direct entry treats GLib warnings as fatal")
  {
    auto const executable = ao::test::currentProbeExecutablePath();
    REQUIRE_FALSE(executable.empty());
    auto const result = ao::test::runProbeProcess(executable, "fatal-warning", std::chrono::seconds{10});
    INFO(result.standardError);
    REQUIRE(result.started);
    REQUIRE_FALSE(result.timedOut);
    CHECK(result.hasFatalTermination());
    CHECK(result.standardError.contains("GTK direct-entry warning probe"));
  }

  TEST_CASE("GtkTestEnvironment - exact frame-submission warning survives only on runtime GTK 4.24.0")
  {
    auto const executable = ao::test::currentProbeExecutablePath();
    REQUIRE_FALSE(executable.empty());
    auto const runtimeGtk4240 =
      ::gtk_get_major_version() == 4 && ::gtk_get_minor_version() == 24 && ::gtk_get_micro_version() == 0;

    for (auto const* const scenario : {"frame-warning", "frame-warning-sized"})
    {
      INFO(scenario);
      auto const result = ao::test::runProbeProcess(executable, scenario, std::chrono::seconds{10});
      INFO(result.standardOutput);
      INFO(result.standardError);

      // Other linked GTK versions must still abort this warning. Do not skip the retirement guard.
      if (runtimeGtk4240)
      {
        REQUIRE(result.hasSuccessfulExit());
        CHECK(result.standardOutput == std::string{scenario} + ": survived=yes\n");
        CHECK(result.standardError.contains("gdk_frame_timings_submitted() called on submitted frame."));
        CHECK(result.standardError.contains("temporary GTK 4.24.0 X11/Cairo frame-submission exception"));
      }
      else
      {
        REQUIRE(result.started);
        REQUIRE_FALSE(result.timedOut);
        CHECK(result.hasFatalTermination());
        CHECK(result.signaled);
        CHECK(result.standardError.contains("GTK frame-warning probe: emitting"));
        CHECK(result.standardError.contains("gdk_frame_timings_submitted() called on submitted frame."));
        CHECK_FALSE(result.standardOutput.contains("survived"));
        CHECK_FALSE(result.standardError.contains("temporary GTK 4.24.0 X11/Cairo frame-submission exception"));
      }
    }
  }

  TEST_CASE("GtkTestEnvironment - nonmatching frame-warning probes remain fatal")
  {
    auto const executable = ao::test::currentProbeExecutablePath();
    REQUIRE_FALSE(executable.empty());

    for (auto const* const scenario : {"frame-warning-other-message",
                                       "frame-warning-other-domain",
                                       "frame-warning-critical",
                                       "frame-warning-error",
                                       "frame-warning-explicit-fatal",
                                       "frame-warning-trailing-byte",
                                       "frame-warning-legacy",
                                       "frame-warning-unowned-display",
                                       "frame-warning-unowned-bus",
                                       "frame-warning-no-display",
                                       "frame-warning-wayland",
                                       "frame-warning-renderer",
                                       "frame-warning-duplicate-domain",
                                       "frame-warning-missing-domain"})
    {
      INFO(scenario);
      auto const result = ao::test::runProbeProcess(executable, scenario, std::chrono::seconds{10});
      INFO(result.standardOutput);
      INFO(result.standardError);
      REQUIRE(result.started);
      REQUIRE_FALSE(result.timedOut);
      CHECK(result.hasFatalTermination());
      CHECK(result.signaled);
      CHECK(result.standardError.contains("GTK frame-warning probe: emitting"));
      CHECK_FALSE(result.standardOutput.contains("survived"));
      CHECK_FALSE(result.standardError.contains("temporary GTK 4.24.0 X11/Cairo frame-submission exception"));
    }
  }
} // namespace ao::gtk::test
