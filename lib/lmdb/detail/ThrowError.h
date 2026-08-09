// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <source_location>

namespace ao::lmdb
{
  void failRead(char const* origin, std::int32_t code, bool transactionOwned);
  [[noreturn]] void throwOnMutationError(char const* origin,
                                         std::int32_t code,
                                         std::source_location location = std::source_location::current());
}
