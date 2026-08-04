// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/winui/app/CommandLine.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace ao::winui
{
  std::wstring quoteCommandLineArgument(std::wstring_view const argument)
  {
    auto quoted = std::wstring{};
    quoted.reserve(argument.size() + 2);
    quoted.push_back(L'"');
    std::size_t backslashCount = 0;

    for (auto const value : argument)
    {
      if (value == L'\\')
      {
        ++backslashCount;
        continue;
      }

      if (value == L'"')
      {
        // Backslashes before a quote are doubled, then one more escapes the
        // quote itself. Backslashes before any other character stay literal.
        quoted.append((backslashCount * 2) + 1, L'\\');
        quoted.push_back(value);
        backslashCount = 0;
        continue;
      }

      quoted.append(backslashCount, L'\\');
      backslashCount = 0;
      quoted.push_back(value);
    }

    // The closing quote would consume an odd trailing backslash, so every
    // trailing backslash is doubled before the delimiter.
    quoted.append(backslashCount * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
  }
} // namespace ao::winui
