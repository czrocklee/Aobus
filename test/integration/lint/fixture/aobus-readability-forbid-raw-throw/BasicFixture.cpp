// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <cstdint>
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
  throw std::runtime_error{"this should fail"};
}

// NEGATIVE
void testGoodThrow()
{
  ao::throwException<std::runtime_error>("this is good");
}

#include <exception>
#include <system_error>

namespace boost::system
{
  class system_error : public std::exception
  {
  public:
    explicit system_error(std::int32_t code)
      : _code{code}
    {
    }

  private:
    std::int32_t _code;
  };
}

namespace
{
  // NEGATIVE
  void testAllowedRawThrows()
  {
    throw std::system_error{std::error_code{}};
    constexpr std::int32_t dummyCode = 42;
    throw boost::system::system_error{dummyCode};
  }
}
