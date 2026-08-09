// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/fatal/RuntimeFatalProbeScenario.h"

#include <catch2/catch_session.hpp>

#include <string_view>

int main(int argc, char* argv[])
{
  if (argc == 3 && std::string_view{argv[1]} == "--aobus-fatal-probe-child")
  {
    return ao::rt::test::runRuntimeFatalProbeScenario(argv[2]);
  }

  auto session = Catch::Session{};
  return session.run(argc, argv);
}
