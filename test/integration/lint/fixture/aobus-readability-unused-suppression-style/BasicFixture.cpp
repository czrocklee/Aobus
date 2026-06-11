// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <cstdint>
#include <tuple>

std::int32_t compute();

void testUnusedSuppression(std::int32_t param)
{
  // POSITIVE
  (void)param;

  // POSITIVE
  static_cast<void>(param);

  // NEGATIVE
  [[maybe_unused]] std::int32_t const param2 = 10;

  // POSITIVE
  (void)compute();

  // POSITIVE
  static_cast<void>(compute());

  // NEGATIVE
  std::ignore = compute();

  std::int32_t const value = compute();

  // POSITIVE
  std::ignore = value;

  // POSITIVE
  std::ignore = param;
}
