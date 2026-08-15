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
   * A file whose length exceeds what it stores allocates less than its length
   * reports, so a footprint taken from `std::filesystem::file_size` overstates
   * what such a file costs. A sparse Windows LMDB data file is exactly that
   * shape, because Windows sets the file's length to the environment's map size
   * while only its committed pages occupy disk. The two figures agree on POSIX,
   * where LMDB appends pages instead of extending the file up front.
   *
   * Returns `0` when the file cannot be inspected, which lets a caller
   * accumulating a directory footprint keep the tolerant accounting it already
   * applies to entries it cannot read.
   */
  std::uint64_t allocatedFileBytes(std::filesystem::path const& path);
} // namespace ao::utility
