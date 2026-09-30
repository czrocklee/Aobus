// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/FileTimestamp.h>

#include <filesystem>

namespace ao::library
{
  /** Converts a native filesystem instant without narrowing it to Unix nanoseconds. */
  Result<FileTimestamp> fileTimestampFromFileTime(std::filesystem::file_time_type time);

  /**
   * Reads and converts a file's last write time. A filesystem failure reports
   * IoError; a conversion failure keeps fileTimestampFromFileTime's error.
   */
  Result<FileTimestamp> lastWriteTimestamp(std::filesystem::path const& path);
} // namespace ao::library
