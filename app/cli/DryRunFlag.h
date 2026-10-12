// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "BooleanFlag.h"

#include <CLI/App.hpp>
#include <CLI/Option.hpp>

namespace ao::cli
{
  inline CLI::Option* addDryRunFlag(CLI::App& cmd)
  {
    return addBooleanFlag(cmd, "--dry-run", "Preview changes without committing them");
  }

  inline bool isDryRun(CLI::Option const* option)
  {
    return option != nullptr && option->count() > 0;
  }
} // namespace ao::cli
