// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <algorithm>
#include <cctype>
#include <string_view>

namespace ao::rt
{
  inline char completionLowerAscii(char ch)
  {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }

  inline bool startsWithCompletionPrefixInsensitive(std::string_view candidate, std::string_view prefix)
  {
    if (prefix.size() > candidate.size())
    {
      return false;
    }

    return std::equal(prefix.begin(),
                      prefix.end(),
                      candidate.begin(),
                      [](char lhs, char rhs) { return completionLowerAscii(lhs) == completionLowerAscii(rhs); });
  }
} // namespace ao::rt
