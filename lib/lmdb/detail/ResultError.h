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
  Error::Code errorCodeFor(std::int32_t code);

  inline std::unexpected<Error> lmdbError(char const* origin,
                                          std::int32_t code,
                                          std::source_location location = std::source_location::current())
  {
    return makeError(errorCodeFor(code), std::format("{}: {}", origin, ::mdb_strerror(code)), location);
  }

  Result<> resultFromCode(char const* origin,
                          std::int32_t code,
                          std::source_location location = std::source_location::current());
} // namespace ao::lmdb
