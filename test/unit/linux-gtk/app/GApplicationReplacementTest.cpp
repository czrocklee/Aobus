// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/fatal/ProbeProcess.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <string_view>

namespace ao::gtk::test
{
  namespace
  {
    ao::test::ProbeProcessResult runGApplicationProbe(std::string_view const scenario)
    {
      constexpr auto kTimeout = std::chrono::seconds{15};
      auto const executablePath = ao::test::siblingProbeExecutablePath("ao_gapplication_probe");

      if (executablePath.empty())
      {
        return {};
      }

      return ao::test::runProbeProcess(executablePath, scenario, kTimeout);
    }

    void requireSuccessfulProbe(ao::test::ProbeProcessResult const& result)
    {
      INFO("launch error: " << result.launchError);
      INFO("standard error: " << result.standardError);
      REQUIRE(result.started);
      REQUIRE_FALSE(result.timedOut);
      REQUIRE(result.hasSuccessfulExit());
      CHECK(result.standardError.empty());
    }
  } // namespace

  TEST_CASE("GApplication replacement - ordinary second instance remains remote without changing the owner",
            "[gtk][integration][gapplication][concurrency]")
  {
    requireOwnedGtkSessionBus();
    auto const result = runGApplicationProbe("ordinary-remote");

    requireSuccessfulProbe(result);
    CHECK(result.standardOutput == "ordinary-remote: remote=yes owner-unchanged=yes\n");
  }

  TEST_CASE("GApplication replacement - replace flag takes ownership from a live replaceable primary",
            "[gtk][integration][gapplication][concurrency]")
  {
    requireOwnedGtkSessionBus();
    auto const result = runGApplicationProbe("replacement");

    requireSuccessfulProbe(result);
    CHECK(result.standardOutput == "replacement: primary=yes owner-changed=yes\n");
  }

  TEST_CASE("GApplication replacement - independent invocations leave a live previous owner untouched",
            "[gtk][integration][gapplication][concurrency]")
  {
    requireOwnedGtkSessionBus();
    auto const result = runGApplicationProbe("invocation-isolation");

    requireSuccessfulProbe(result);
    CHECK(result.standardOutput == "invocation-isolation: ordinary=yes replacement=yes previous-owner-unchanged=yes\n");
  }

  TEST_CASE("GApplication replacement - probe refuses unowned session buses before connecting",
            "[gtk][integration][gapplication]")
  {
    for (auto const* const scenario : {"unowned-bus", "mismatched-bus", "unowned-instance"})
    {
      INFO(scenario);
      auto const result = runGApplicationProbe(scenario);
      INFO("launch error: " << result.launchError);
      INFO("standard error: " << result.standardError);
      REQUIRE(result.started);
      REQUIRE_FALSE(result.timedOut);
      REQUIRE(result.exited);
      CHECK(result.exitCode == 1);
      CHECK(result.standardOutput.empty());
      CHECK(result.standardError ==
            "GApplication replacement probe failed: a portal-owned session bus is required; run ./ao test --gtk\n");
    }
  }
} // namespace ao::gtk::test
