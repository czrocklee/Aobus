// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ResultError.h"

#include <ao/Error.h>

#include <lmdb.h>

#include <cstdint>
#include <source_location>

namespace ao::lmdb
{
  Error::Code errorCodeFor(std::int32_t code)
  {
    if (code == MDB_NOTFOUND)
    {
      return Error::Code::NotFound;
    }

    if (code == MDB_KEYEXIST)
    {
      return Error::Code::Conflict;
    }

    if (code == MDB_MAP_FULL)
    {
      // The map ran out of room, which a larger one would have admitted. This is
      // its own code because the remedy differs from every other storage
      // failure: a caller may reopen with more capacity and repeat the work,
      // while a full disk or an exhausted id space would only fail again.
      return Error::Code::StorageFull;
    }

    if (code == MDB_MAP_RESIZED)
    {
      // Another process committed past this process's map, so this environment's
      // mapping is stale. Reopening adopts the larger size; nothing else can.
      return Error::Code::InvalidState;
    }

    return Error::Code::IoError;
  }

  Result<> resultFromCode(char const* origin, std::int32_t code, std::source_location location)
  {
    if (code == MDB_SUCCESS)
    {
      return {};
    }

    return lmdbError(origin, code, location);
  }
} // namespace ao::lmdb
