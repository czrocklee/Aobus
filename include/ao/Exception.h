// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <concepts>
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
    explicit Exception(std::string what, std::source_location loc = std::source_location::current())
      : _what{std::move(what)}, _location{loc}
    {
    }

    char const* file() const noexcept { return _location.file_name(); }

    std::int32_t line() const noexcept { return static_cast<std::int32_t>(_location.line()); }

    char const* what() const noexcept override { return _what.c_str(); }

    std::source_location const& location() const noexcept { return _location; }

  private:
    std::string _what;
    std::source_location _location;
  };

  /**
   * @brief Helper to capture source location alongside a format string.
   */
  template<typename... Args>
  struct FormatWithLocation
  {
    std::format_string<Args...> fmt;
    std::source_location loc;

    template<typename T>
    consteval FormatWithLocation(T const& fmtStr, std::source_location location = std::source_location::current())
      : fmt{fmtStr}, loc{location}
    {
    }
  };

  /**
   * @brief Throws a formatted exception with captured source location and compile-time format check.
   * Selected when one or more formatting arguments are provided.
   */
  template<typename ExceptionType, typename... Args>
    requires(sizeof...(Args) > 0 && std::derived_from<ExceptionType, std::exception>)
  [[noreturn]] void throwException(FormatWithLocation<std::type_identity_t<Args>...> fmt, Args&&... args)
  {
    throw ExceptionType{std::format(fmt.fmt, std::forward<Args>(args)...), fmt.loc};
  }

  /**
   * @brief Throws a simple string exception with captured source location.
   * Selected when no additional formatting arguments are provided.
   */
  template<typename ExceptionType>
    requires std::derived_from<ExceptionType, std::exception>
  [[noreturn]] void throwException(std::string_view what, std::source_location loc = std::source_location::current())
  {
    throw ExceptionType{std::string{what}, loc};
  }
} // namespace ao
