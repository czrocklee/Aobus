// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

namespace ao::cli
{
  class CliRuntime;

  void runScan(CliRuntime& cli, bool dryRun, bool verbose, bool deferFingerprint = false);
} // namespace ao::cli
