// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <CLI/CLI.hpp>

namespace ao::cli
{
  class CliRuntime;

  void runScan(CliRuntime& cli, bool dryRun, bool verbose, bool deferFingerprint = false);
  void configureScanCommand(CLI::App& app, CliRuntime& cli);
} // namespace ao::cli
