// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "InitCommand.h"

#include "DryRunFlag.h"
#include "ScanCommand.h"

#include <CLI/App.hpp>

namespace ao::cli
{
  void configureInitCommand(CLI::App& app, CliRuntime& cli)
  {
    auto* const cmd = app.add_subcommand("init", "Initialize library and scan the music root");
    auto* const dryRun = addDryRunFlag(*cmd);
    cmd->callback([&cli, dryRun] { runScan(cli, isDryRun(dryRun), false, false); });
  }
} // namespace ao::cli
