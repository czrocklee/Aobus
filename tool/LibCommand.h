// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <CLI/CLI.hpp>
#include <rs/core/MusicLibrary.h>

namespace rs::tool
{
  void setupLibCommand(CLI::App& app, core::MusicLibrary& ml);
}
