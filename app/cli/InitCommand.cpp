// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "InitCommand.h"

#include "ScanCommand.h"

#include <CLI/App.hpp>

namespace ao::cli
{
  void setupInitCommand(CLI::App& app, CliContext& context)
  {
    auto* const cmd = app.add_subcommand("init", "Initialize library and scan the music root");
    cmd->callback([&context] { runScan(context, false, false); });
  }
} // namespace ao::cli
