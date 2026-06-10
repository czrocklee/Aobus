// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <cstdint>
#include <iostream>

void testLambdaParams()
{
  // POSITIVE: FIX-TO: [[maybe_unused]] auto invalidLambda = [] { std::cout << "Hi"; };
  [[maybe_unused]] auto invalidLambda = []() { std::cout << "Hi"; };

  // NEGATIVE
  [[maybe_unused]] auto validLambda = [] { std::cout << "Hi"; };

  // NEGATIVE
  [[maybe_unused]] auto paramsLambda = [](std::int32_t x) { std::cout << x; };
}
