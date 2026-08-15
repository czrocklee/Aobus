// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/lmdb/Environment.h>

#include <cstdint>
#include <filesystem>

namespace ao::lmdb::detail
{
  /// What the environment being opened is allowed to do to its data file.
  enum class DataFileAccess : std::uint8_t
  {
    /// The environment will write, so the file may be created and marked sparse.
    ReadWrite,
    /**
     * The environment only reads, so the file must be left exactly as it is.
     *
     * A read-only open must not create a database that was not there, must not
     * ask for write permission on media that refuses it, and must not mark a
     * file sparse, which is itself a mutation. The allocation is then reported
     * from what the file already says rather than from what was arranged.
     */
    ReadOnly,
  };

  /**
   * @brief Prepares an environment's data file before LMDB maps it.
   *
   * Windows extends the data file to the configured map size before creating the
   * mapping, which allocates the whole map before the environment holds one
   * record. This makes that file sparse first, so its length still reports the
   * map while its allocation follows committed pages. POSIX needs nothing: with
   * `MDB_WRITEMAP` disabled the mapping never extends the file, and LMDB appends
   * pages as it writes them, so length and allocation both follow committed use.
   *
   * The reported allocation tells the caller which of the two it got, because a
   * filesystem that cannot hold a hole turns the map size into disk usage and a
   * capacity decision has to know that before choosing one.
   *
   * Marking a file sparse does not release clusters it has already allocated, so
   * a data file written by an earlier dense build keeps its allocation until
   * something reclaims the unused tail. Reclaiming it needs the environment
   * closed and the committed high water known, which only a caller that reopens
   * the environment can arrange.
   *
   * @param directory Environment directory that holds or will receive the data
   *                  file. It must already exist.
   * @param access    Whether the environment may modify the file at all.
   */
  Result<MapAllocation> prepareEnvironmentDataFile(std::filesystem::path const& directory, DataFileAccess access);
} // namespace ao::lmdb::detail
