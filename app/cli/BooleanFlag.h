// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <CLI/App.hpp>
#include <CLI/Option.hpp>

#include <string>
#include <utility>

namespace ao::cli
{
  /**
   * Adds a presence flag whose callers test `count() > 0`.
   *
   * CLI11 would otherwise record an attached `--flag=false` as one more
   * occurrence, so a negating value would still enable the flag. Disabling the
   * override rejects every attached value other than `true` with CLI11's
   * argument-mismatch status before any command callback runs.
   */
  inline CLI::Option* addBooleanFlag(CLI::App& cmd, std::string name, std::string description)
  {
    return cmd.add_flag(std::move(name), std::move(description))->disable_flag_override();
  }
} // namespace ao::cli
