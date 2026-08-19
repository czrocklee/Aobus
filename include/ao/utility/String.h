// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <string>
#include <string_view>

namespace ao::utility
{
  constexpr bool isAsciiAlpha(char const ch) noexcept
  {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
  }

  constexpr bool isAsciiDigit(char const ch) noexcept
  {
    return ch >= '0' && ch <= '9';
  }

  constexpr bool isAsciiAlphaNumeric(char const ch) noexcept
  {
    return isAsciiAlpha(ch) || isAsciiDigit(ch);
  }

  constexpr bool isAsciiWhitespace(char const ch) noexcept
  {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
  }

  constexpr bool isAsciiPrintable(char const ch) noexcept
  {
    return ch >= ' ' && ch <= '~';
  }

  constexpr char toAsciiLower(char const ch) noexcept
  {
    return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + ('a' - 'A')) : ch;
  }

  constexpr char toAsciiUpper(char const ch) noexcept
  {
    return ch >= 'a' && ch <= 'z' ? static_cast<char>(ch - ('a' - 'A')) : ch;
  }

  // These transformations deliberately recognize ASCII only. Bytes belonging to
  // UTF-8 code units are preserved verbatim; Unicode case folding is a separate operation.
  std::string toLower(std::string_view text);
  std::string_view trim(std::string_view text);
} // namespace ao::utility
