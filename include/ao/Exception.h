// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <cstdint>
#include <exception>
#include <format>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ao
{
  /**
   * @brief Base exception class for Aobus.
   */
  class Exception : public std::exception
  {
  public:
    Exception(std::string what, char const* file, std::int32_t line)
      : _what{std::move(what)}, _file{file}, _line{line}
    {
    }

    char const* file() const { return _file; }

    std::int32_t line() const { return _line; }

    char const* what() const noexcept override { return _what.c_str(); }

  private:
    std::string _what;
    char const* _file;
    std::int32_t _line;
  };

  /**
   * @brief Helper to capture source location alongside a format string.
   */
  template <typename... Args>
  struct FormatWithLocation
  {
    std::format_string<Args...> fmt;
    std::source_location loc;

    template <typename T>
    consteval FormatWithLocation(T const& fmtStr, std::source_location location = std::source_location::current())
      : fmt{fmtStr}, loc{location}
    {
    }
  };

  /**
   * @brief Throws a formatted exception with captured source location and compile-time format check.
   * Selected when one or more formatting arguments are provided.
   */
  template <typename ExceptionType, typename... Args>
  requires (sizeof...(Args) > 0)
  [[noreturn]] void throwException(FormatWithLocation<std::type_identity_t<Args>...> fmt, Args&&... args)
  {
    throw ExceptionType{std::format(fmt.fmt, std::forward<Args>(args)...),
                        fmt.loc.file_name(),
                        static_cast<std::int32_t>(fmt.loc.line())};
  }

  /**
   * @brief Throws a simple string exception with captured source location.
   * Selected when no additional formatting arguments are provided.
   */
  template <typename ExceptionType>
  [[noreturn]] void throwException(std::string_view what,
                                   std::source_location loc = std::source_location::current())
  {
    throw ExceptionType{std::string{what}, loc.file_name(), static_cast<std::int32_t>(loc.line())};
  }
}
