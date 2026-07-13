// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace ao::utility
{
  enum class AtomicFilePermissions : std::int16_t
  {
    Default = -1,
    OwnerReadWrite = 0600,
  };

  /**
   * @brief Write data to targetPath atomically using a temp file and rename.
   *
   * The temp file is created in the target directory, fully written, flushed,
   * closed, and replaced through the platform rename operation. Non-Windows
   * builds attempt a best-effort parent-directory fsync after replacement.
   * Temporary cleanup after failure is also best-effort.
   *
   * On Windows, permission values are advisory; the replaced file inherits the
   * destination directory ACLs. See
   * doc/spec/persistence/atomic-replacement.md for the exact platform and
   * crash-durability boundaries.
   */
  Result<> writeAtomically(std::filesystem::path const& targetPath,
                           std::string_view data,
                           AtomicFilePermissions permissions = AtomicFilePermissions::OwnerReadWrite);
} // namespace ao::utility
