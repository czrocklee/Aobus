// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <format>
#include <lmdb.h>
#include <rs/Exception.h>
#include <stdexcept>
#include <string>

namespace rs::lmdb
{
  inline void throwOnError(char const* origin, int code)
  {
    if (code != MDB_SUCCESS)
    {
      RS_THROW_FORMAT(rs::Exception, "{}: {}", origin, ::mdb_strerror(code));
    }
  }
}
