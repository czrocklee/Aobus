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
   * The temp file is created in the same directory as the target, optionally
   * chmod'd, fully written, fsync'd, and renamed over the target. The parent
   * directory is fsync'd on success. Temp files are removed on failure.
   * On Windows, permission values are advisory; the replaced file inherits the
   * destination directory ACLs.
   */
  Result<> writeAtomically(std::filesystem::path const& targetPath,
                           std::string_view data,
                           AtomicFilePermissions permissions = AtomicFilePermissions::OwnerReadWrite);
} // namespace ao::utility
