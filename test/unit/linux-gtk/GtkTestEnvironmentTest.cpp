// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "GtkApplicationTestSupport.h"
#include "test/fatal/ProbeProcess.h"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

namespace ao::gtk::test
{
  TEST_CASE("GtkTestEnvironment - bus admission requires a matching owned Unix endpoint",
            "[gtk][unit][test-environment]")
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

  TEST_CASE("GtkTestEnvironment - direct entry disables unowned buses before test dispatch",
            "[gtk][integration][test-environment]")
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

  TEST_CASE("GtkTestEnvironment - direct entry treats GLib warnings as fatal", "[gtk][integration][test-environment]")
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
} // namespace ao::gtk::test
