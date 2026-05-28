// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <cstdint>

void testUnusedSuppression(std::int32_t param)
{
  // POSITIVE
  (void)param;

  // POSITIVE
  static_cast<void>(param);

  // NEGATIVE
  [[maybe_unused]] std::int32_t const param2 = 10;
}
