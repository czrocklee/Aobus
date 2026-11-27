// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/utility/String.h>

#include <algorithm>
#include <string>
#include <string_view>

namespace ao::utility
{
  bool isEqualIgnoringAsciiCase(std::string_view lhs, std::string_view rhs) noexcept
  {
    return std::ranges::equal(lhs, rhs, {}, toAsciiLower, toAsciiLower);
  }

  std::string toLower(std::string_view text)
  {
    auto result = std::string{};
    result.reserve(text.size());

    for (auto const ch : text)
    {
      result.push_back(toAsciiLower(ch));
    }

    return result;
  }

  std::string_view trim(std::string_view text)
  {
    while (!text.empty() && isAsciiWhitespace(text.front()))
    {
      text.remove_prefix(1);
    }

    while (!text.empty() && isAsciiWhitespace(text.back()))
    {
      text.remove_suffix(1);
    }

    return text;
  }
} // namespace ao::utility
