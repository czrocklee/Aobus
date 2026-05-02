// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <ao/Exception.h>
#include <format>
#include <lmdb.h>
#include <stdexcept>
#include <string>

namespace ao::lmdb
{
  inline void throwOnError(char const* origin, int code)
  {
    if (code != MDB_SUCCESS)
    {
      AO_THROW_FORMAT(ao::Exception, "{}: {}", origin, ::mdb_strerror(code));
    }
  }
}
