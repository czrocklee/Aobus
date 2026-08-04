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

template<typename... Args>
void testVariadicLambda()
{
  // NEGATIVE
  [[maybe_unused]] auto variadicLambda = [](Args... args) { return sizeof...(args); };
}

template<typename T>
void testTemplatedEmptyLambda()
{
  // POSITIVE: FIX-TO: [[maybe_unused]] auto invalidTemplatedLambda = [] { return sizeof(T); };
  [[maybe_unused]] auto invalidTemplatedLambda = []() { return sizeof(T); };
}

template void testVariadicLambda<>();
template void testVariadicLambda<std::int32_t>();
template void testTemplatedEmptyLambda<std::int32_t>();
