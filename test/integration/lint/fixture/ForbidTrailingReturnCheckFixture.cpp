// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <cstdint>

// POSITIVE
auto testTrailing() -> std::int32_t
{
  return 10;
}

// POSITIVE
auto testNecessaryTrailing() -> decltype(auto)
{
  static std::int32_t x = 5;
  return (x);
}

// NEGATIVE
auto lambdaWithTrailing = [](std::int32_t x) -> double { return x * 1.0; };

// NEGATIVE
template<typename T>
struct DeductionDemo
{
  DeductionDemo(T) {}
};
DeductionDemo(std::int32_t) -> DeductionDemo<double>;
