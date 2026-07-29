// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/completion/CompletionText.h>

#include <algorithm>
#include <string_view>

namespace ao::rt
{
  bool startsWithCompletionPrefixInsensitive(std::string_view candidate, std::string_view prefix)
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
