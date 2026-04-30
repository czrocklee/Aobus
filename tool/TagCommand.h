// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <CLI/CLI.hpp>
#include <rs/library/MusicLibrary.h>

namespace rs::tool
{
  void setupTagCommand(CLI::App& app, rs::library::MusicLibrary& ml);
}
