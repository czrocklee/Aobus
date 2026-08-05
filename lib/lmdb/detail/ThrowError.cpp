// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ThrowError.h"

#include "ResultError.h"
#include "TransactionFailure.h"
#include <ao/Error.h>
#include <ao/Exception.h>
#include <ao/ExceptionFormat.h>

#include <lmdb.h>

#include <cstdint>
#include <format>
#include <source_location>

namespace ao::lmdb
{
  void throwOnError(char const* origin, std::int32_t code)
  {
    if (code != MDB_SUCCESS)
    {
      throwException<Exception>("{}: {}", origin, ::mdb_strerror(code));
    }
  }

  [[noreturn]] void throwOnMutationError(char const* origin, std::int32_t code, std::source_location location)
  {
    throw detail::TransactionFailure{Error{.code = errorCodeFor(code),
                                           .message = std::format("{}: {}", origin, ::mdb_strerror(code)),
                                           .location = location}};
  }
} // namespace ao::lmdb
