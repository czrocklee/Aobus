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
