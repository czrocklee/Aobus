// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "EnvironmentDataFile.h"
#include <ao/Error.h>

#include <filesystem>

namespace ao::lmdb::detail
{
  Result<> prepareEnvironmentDataFile(std::filesystem::path const& /*directory*/)
  {
    // Nothing to prepare. Without MDB_WRITEMAP the POSIX mapping never extends
    // the data file; LMDB extends it only by writing pages, at commit or when a
    // large transaction spills, so the map costs no blocks and an aborted
    // transaction leaves only what it wrote. Creating the file early would only
    // take work away from LMDB's own open path.
    return {};
  }
} // namespace ao::lmdb::detail
