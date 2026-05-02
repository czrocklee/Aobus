// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <CLI/CLI.hpp>
#include <ao/library/MusicLibrary.h>

namespace ao::tool
{
  void setupLibCommand(CLI::App& app, ao::library::MusicLibrary& ml);
}
