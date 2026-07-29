// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cctype>
#include <string_view>

namespace ao::rt
{
  inline char completionLowerAscii(char ch)
  {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }

  bool startsWithCompletionPrefixInsensitive(std::string_view candidate, std::string_view prefix);
} // namespace ao::rt
