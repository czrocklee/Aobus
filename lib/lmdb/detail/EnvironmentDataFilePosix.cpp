// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "EnvironmentDataFile.h"
#include <ao/Error.h>
#include <ao/lmdb/Environment.h>

#include <filesystem>

namespace ao::lmdb::detail
{
  Result<MapAllocation> prepareEnvironmentDataFile(std::filesystem::path const& /*directory*/,
                                                   DataFileAccess const /*access*/)
  {
    // Nothing to prepare, for either access. Without MDB_WRITEMAP the POSIX
    // mapping never extends the data file, so LMDB writes pages as it needs them
    // and the rest of the map costs no blocks. Creating the file early would
    // only take work away from LMDB's own open path, and because this touches
    // nothing it is already safe for a read-only environment.
    return MapAllocation::OnDemand;
  }
} // namespace ao::lmdb::detail
