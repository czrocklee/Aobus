// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <CLI/CLI.hpp>

namespace ao::rt
{
  class CoreRuntime;
}

namespace ao::cli
{
  void setupTrackCommand(CLI::App& app, rt::CoreRuntime& runtime);
}
