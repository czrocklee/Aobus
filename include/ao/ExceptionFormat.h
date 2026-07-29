// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Exception.h>

#include <concepts>
#include <exception>
#include <format>
#include <source_location>
#include <type_traits>
#include <utility>

namespace ao
{
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
} // namespace ao
