// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>

namespace ao::lmdb
{
  void throwOnError(char const* origin, std::int32_t code);
}
