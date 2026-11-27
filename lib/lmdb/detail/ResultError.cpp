// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ResultError.h"

#include <ao/Error.h>

#include <lmdb.h>

#include <cstdint>
#include <expected>
#include <format>
#include <source_location>

namespace ao::lmdb
{
  namespace
  {
    Error::Code errorCodeFor(std::int32_t const code) noexcept
    {
      switch (code)
      {
        case MDB_NOTFOUND: return Error::Code::NotFound;
        case MDB_KEYEXIST: return Error::Code::Conflict;
        // A larger map can admit the same work; a full disk is still IoError.
        case MDB_MAP_FULL: return Error::Code::StorageFull;
        // Another process grew past this mapping; reopen to adopt its size.
        case MDB_MAP_RESIZED: return Error::Code::InvalidState;
        default: return Error::Code::IoError;
      }
    }
  } // namespace

  Error lmdbError(char const* origin, std::int32_t const code, std::source_location const location)
  {
    return Error{
      .code = errorCodeFor(code), .message = std::format("{}: {}", origin, ::mdb_strerror(code)), .location = location};
  }

  Result<> resultFromCode(char const* origin, std::int32_t code, std::source_location location)
  {
    if (code == MDB_SUCCESS)
    {
      return {};
    }

    return std::unexpected{lmdbError(origin, code, location)};
  }
} // namespace ao::lmdb
