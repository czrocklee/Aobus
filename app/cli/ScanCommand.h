// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <CLI/CLI.hpp>

namespace ao::cli
{
  class CliContext;

  void runScan(CliContext& context, bool dryRun, bool verbose);
  void setupScanCommand(CLI::App& app, CliContext& context);
} // namespace ao::cli
