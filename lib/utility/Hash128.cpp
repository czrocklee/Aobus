// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/utility/Hash128.h>

#include "Hex.h"

#include <string>

namespace ao::utility
{
  std::string hash128Hex(Hash128 const& hash)
  {
    return detail::lowerHex(hash.bytes);
  }
} // namespace ao::utility
