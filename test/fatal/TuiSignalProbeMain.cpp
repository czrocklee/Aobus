// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/fatal/TuiSignalProbeScenario.h"

#ifdef _WIN32
#include <cstdio>
#endif
#include <print>
#include <string_view>

int main(int argc, char* argv[])
{
  if (argc == 3 && std::string_view{argv[1]} == "--aobus-probe-child")
  {
    return ao::tui::test::runTuiSignalProbeScenario(argv[2]);
  }

  std::println(stderr, "Usage: ao_tui_signal_probe --aobus-probe-child <scenario>");
  return 2;
}
