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

// A subsystem throw<Domain>Error helper inside the ao namespace may raise a raw
// throw directly: its name begins with "throw" followed by an upper-case letter,
// so it is a sanctioned throwing helper just like ao::throwException.
namespace ao::audio::detail
{
  [[noreturn]] void throwDecoderError(std::string_view message)
  {
    // NEGATIVE
    throw std::runtime_error{std::string{message}};
  }
}

// The exemption is limited to the ao namespace tree: a throw<Domain>Error-shaped
// helper outside ao is still held to the rule.
namespace other
{
  [[noreturn]] void throwOutsideAo(std::string_view message)
  {
    // POSITIVE
    throw std::runtime_error{std::string{message}};
  }
}

// A function in ao whose name merely starts with the letters "throw" but lacks the
// upper-case word boundary of a helper ("throwaway") is still held to the rule.
namespace ao
{
  void throwaway()
  {
    // POSITIVE
    throw std::runtime_error{"this should fail"};
  }
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
