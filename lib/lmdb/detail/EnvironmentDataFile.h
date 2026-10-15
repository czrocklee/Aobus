// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <filesystem>

namespace ao::lmdb::detail
{
  /**
   * @brief Prepares a writable environment's data file before LMDB maps it.
   *
   * Windows LMDB commits each page it allocates from the end of the map before
   * anything is written to it, and committing a page of a file-backed section
   * extends the file. A transaction that aborts therefore still leaves the file
   * as long as the highest page it allocated. This marks the file sparse first,
   * so that extension stays a hole and allocation follows what was written. A
   * volume that cannot hold a hole, such as exFAT, is left as it is, and there
   * an aborted transaction's extension stays allocated until a later
   * transaction reuses its pages.
   *
   * POSIX needs nothing: with `MDB_WRITEMAP` disabled LMDB extends the file only
   * by writing pages to it, so an aborted transaction leaves only the pages it
   * wrote, which is none unless it spilled.
   *
   * Only a writable environment calls this. Creating the file, asking for write
   * access, and changing its attributes are all mutations a read-only open must
   * not make.
   *
   * Marking a file sparse does not release clusters it has already allocated,
   * so a data file extended before it was marked keeps that allocation until a
   * later transaction writes over it.
   *
   * @param directory Environment directory that holds or will receive the data
   *                  file. It must already exist.
   */
  Result<> prepareEnvironmentDataFile(std::filesystem::path const& directory);
} // namespace ao::lmdb::detail
