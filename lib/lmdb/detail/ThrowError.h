// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <source_location>

namespace ao::lmdb
{
  void throwOnError(char const* origin, std::int32_t code);
  [[noreturn]] void throwOnMutationError(char const* origin,
                                         std::int32_t code,
                                         std::source_location location = std::source_location::current());
}
