// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/fatal/ProbeProcess.h"

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
    auto const result = runGApplicationProbe("ordinary-remote");

    requireSuccessfulProbe(result);
    CHECK(result.standardOutput == "ordinary-remote: remote=yes owner-unchanged=yes\n");
  }

  TEST_CASE("GApplication replacement - replace flag takes ownership from a live replaceable primary",
            "[gtk][integration][gapplication][concurrency]")
  {
    auto const result = runGApplicationProbe("replacement");

    requireSuccessfulProbe(result);
    CHECK(result.standardOutput == "replacement: primary=yes owner-changed=yes\n");
  }
} // namespace ao::gtk::test
