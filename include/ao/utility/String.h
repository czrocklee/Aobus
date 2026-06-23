// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace ao::utility
{
  inline std::string toLower(std::string_view text)
  {
    auto result = std::string{};
    result.reserve(text.size());

    for (auto const ch : text)
    {
      result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }

    return result;
  }

  inline std::string_view trim(std::string_view text)
  {
    auto const isSpace = [](char ch) { return std::isspace(static_cast<unsigned char>(ch)) != 0; };

    while (!text.empty() && isSpace(text.front()))
    {
      text.remove_prefix(1);
    }

    while (!text.empty() && isSpace(text.back()))
    {
      text.remove_suffix(1);
    }

    return text;
  }
} // namespace ao::utility
