// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <lmdb.h>
#include <stdexcept>
#include <string>

namespace rs::lmdb
{
  inline void throwOnError(char const* origin, int code)
  {
    if (code != MDB_SUCCESS)
    {
      throw std::runtime_error{std::string{origin} + ": " + mdb_strerror(code)};
    }
  }
}
