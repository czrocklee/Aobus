// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "PerformanceReport.h"
#include <ao/Error.h>

#include <catch2/catch_config.hpp>
#include <catch2/catch_session.hpp>

#include <iostream>
#include <print>

int main(int argc, char* argv[])
{
  auto session = Catch::Session{};
  auto const status = session.run(argc, argv);
  // Informational commands have no workload to publish. Failed workloads may
  // leave diagnostic v2 output, but must not publish a successful v1 artifact.
  if (auto const& config = session.configData(); status != 0 || config.showHelp || config.libIdentify ||
                                                 config.listTests || config.listTags || config.listReporters ||
                                                 config.listListeners)
  {
    return status;
  }

  if (auto reportRes = ao::rt::test::writeRequestedBaselineReport(); !reportRes)
  {
    std::println(std::cerr, "Aobus performance baseline: {}", reportRes.error().message);
    return 1;
  }

  return 0;
}
