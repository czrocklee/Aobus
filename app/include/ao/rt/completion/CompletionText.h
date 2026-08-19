// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/utility/String.h>

#include <string_view>

namespace ao::rt
{
  inline char completionLowerAscii(char ch)
  {
    return utility::toAsciiLower(ch);
  }

  bool startsWithCompletionPrefixInsensitive(std::string_view candidate, std::string_view prefix);
} // namespace ao::rt
