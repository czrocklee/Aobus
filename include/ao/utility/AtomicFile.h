// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <filesystem>
#include <string_view>

namespace ao::utility
{
  /**
   * @brief Write data to targetPath atomically using a temp file and rename.
   *
   * A private-user temp file is created in the target directory, fully written,
   * flushed, closed, and replaced through the platform rename operation.
   * Non-Windows builds attempt a best-effort parent-directory fsync after
   * replacement. An uncommitted temp file owns best-effort RAII cleanup.
   *
   * Success means the platform replacement call succeeded; it does not promise
   * absolute power-loss durability. See
   * doc/spec/persistence/atomic-replacement.md for the exact boundary.
   */
  Result<> writeAtomically(std::filesystem::path const& targetPath, std::string_view data);
} // namespace ao::utility
