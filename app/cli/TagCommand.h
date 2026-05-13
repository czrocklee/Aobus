// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <CLI/CLI.hpp>
#include <ao/library/MusicLibrary.h>

namespace ao::cli
{
  void setupTagCommand(CLI::App& app, library::MusicLibrary& ml);
}
