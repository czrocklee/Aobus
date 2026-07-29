// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ThrowError.h"

#include <ao/Exception.h>
#include <ao/ExceptionFormat.h>

#include <lmdb.h>

#include <cstdint>

namespace ao::lmdb
{
  void throwOnError(char const* origin, std::int32_t code)
  {
    if (code != MDB_SUCCESS)
    {
      throwException<Exception>("{}: {}", origin, ::mdb_strerror(code));
    }
  }
} // namespace ao::lmdb
