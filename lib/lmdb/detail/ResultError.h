// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <cstdint>
#include <source_location>

namespace ao::lmdb
{
  Error lmdbError(char const* origin,
                  std::int32_t code,
                  std::source_location location = std::source_location::current());

  Result<> resultFromCode(char const* origin,
                          std::int32_t code,
                          std::source_location location = std::source_location::current());
} // namespace ao::lmdb
