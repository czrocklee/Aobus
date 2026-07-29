// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/rt/library/LibraryAuthoring.h>

#include <CLI/CLI.hpp>

namespace ao::rt
{
  class CoreRuntime;
}

namespace ao::cli
{
  class CliRuntime;

  Result<> validateListOrderCommandStatus(rt::ListOrderAuthoringStatus status);
  void configureListCommand(CLI::App& app, CliRuntime& cli);
}
