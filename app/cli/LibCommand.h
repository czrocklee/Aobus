// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <CLI/CLI.hpp>

namespace ao::rt
{
  class CoreRuntime;
}

namespace ao::cli
{
  class CliContext;

  void configureLibCommand(CLI::App& app, CliContext& context);
}
