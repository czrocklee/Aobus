// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

// FIX-TO: #include <cstddef>\n\n#include "TestHelpers.h"
#include "TestHelpers.h"

void testCast()
{
  // POSITIVE: FIX-TO: auto castVal = static_cast<std::ptrdiff_t>(10);
  auto castVal = static_cast<long>(10);
}
