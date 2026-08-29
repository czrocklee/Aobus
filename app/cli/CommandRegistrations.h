// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

namespace CLI
{
  class App;
}

namespace ao::cli
{
  class CliRuntime;

  void configureInitCommand(CLI::App& app, CliRuntime& cli);
  void configureLibCommand(CLI::App& app, CliRuntime& cli);
  void configureListCommand(CLI::App& app, CliRuntime& cli);
  void configureScanCommand(CLI::App& app, CliRuntime& cli);
  void configureTagCommand(CLI::App& app, CliRuntime& cli);
  void configureTrackCommand(CLI::App& app, CliRuntime& cli);
} // namespace ao::cli
