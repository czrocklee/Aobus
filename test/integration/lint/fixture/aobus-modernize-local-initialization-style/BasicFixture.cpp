// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TestHelpers.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

void testLocalInitialization()
{
  using namespace std::string_literals;
  using namespace std::string_view_literals;

  // POSITIVE
  LocalFoo explicitLocal{10};

  // POSITIVE
  std::string explicitStr("hello");

  // POSITIVE
  std::string_view explicitSv("world");

  // POSITIVE
  std::vector<int> explicitVec(10, 2);

  // POSITIVE
  std::int32_t bracedPrimitive{5};

  // NEGATIVE
  [[maybe_unused]] auto modernVal = std::int32_t{5};
  [[maybe_unused]] auto optStr = "hello"s;
  [[maybe_unused]] auto optSv = "world"sv;
  [[maybe_unused]] auto optVec = std::vector<int>(10, 2);
  [[maybe_unused]] std::int32_t optPrimitive = 5;

  // NEGATIVE
  [[maybe_unused]] std::int32_t* optPointer = nullptr;
}
