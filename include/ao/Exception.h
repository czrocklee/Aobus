// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <concepts>
#include <cstdint>
#include <exception>
#include <source_location>
#include <string>
#include <string_view>

namespace ao
{
  /**
   * @brief Base exception class for Aobus.
   */
  class Exception : public std::exception
  {
  public:
    explicit Exception(std::string what, std::source_location loc = std::source_location::current());

    char const* file() const noexcept;
    std::int32_t line() const noexcept;
    char const* what() const noexcept override;
    std::source_location const& location() const noexcept;

  private:
    std::string _what;
    std::source_location _location;
  };

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
