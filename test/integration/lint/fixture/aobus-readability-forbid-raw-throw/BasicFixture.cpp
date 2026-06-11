// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <stdexcept>
#include <string_view>

namespace ao
{
  template<typename ExceptionType, typename... Args>
  void throwException(std::string_view fmt, Args&&... args)
  {
    // NEGATIVE
    throw ExceptionType{"dummy"};
  }
}

void testRawThrow()
{
  // POSITIVE
  throw std::runtime_error("this should fail");
}

// NEGATIVE
void testGoodThrow()
{
  ao::throwException<std::runtime_error>("this is good");
}
