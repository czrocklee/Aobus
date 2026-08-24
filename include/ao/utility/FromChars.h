// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <charconv>
#include <concepts>

namespace ao::utility
{
  /**
   * @brief std::from_chars for integral types.
   */
  template<std::integral T>
  std::from_chars_result fromChars(char const* first, char const* last, T& value)
  {
    return std::from_chars(first, last, value);
  }

  /**
   * @brief Locale-independent decimal parsing with std::from_chars semantics.
   *
   * The floating-point overloads are implemented with the governed fast_float
   * dependency so parsing behavior is identical across supported platforms.
   * The output is unchanged when parsing fails.
   */
  std::from_chars_result fromChars(char const* first, char const* last, float& value);
  std::from_chars_result fromChars(char const* first, char const* last, double& value);
} // namespace ao::utility
