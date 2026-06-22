// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <lmdb.h>

#include <cstdint>
#include <expected>
#include <format>
#include <source_location>

namespace ao::lmdb
{
  inline Error::Code errorCodeFor(std::int32_t code)
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

  inline std::unexpected<Error> lmdbError(char const* origin,
                                          std::int32_t code,
                                          std::source_location location = std::source_location::current())
  {
    return makeError(errorCodeFor(code), std::format("{}: {}", origin, ::mdb_strerror(code)), location);
  }

  inline Result<> resultFromCode(char const* origin,
                                 std::int32_t code,
                                 std::source_location location = std::source_location::current())
  {
    if (code == MDB_SUCCESS)
    {
      return {};
    }

    return lmdbError(origin, code, location);
  }
} // namespace ao::lmdb
