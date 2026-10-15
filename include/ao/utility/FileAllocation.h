// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <filesystem>

namespace ao::utility
{
  /**
   * @brief Bytes the filesystem has allocated for @p path.
   *
   * This is what the file occupies on its volume, which need not be what
   * `std::filesystem::file_size` reports: the filesystem rounds allocation up
   * to whole clusters, and a file with holes allocates less than its length.
   *
   * Returns `0` when the file cannot be inspected, which lets a caller
   * accumulating a directory footprint keep the tolerant accounting it already
   * applies to entries it cannot read.
   */
  std::uint64_t allocatedFileBytes(std::filesystem::path const& path);
} // namespace ao::utility
