// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <string>
#include <string_view>

namespace ao::winui
{
  /** Quote one UTF-16 argument for the CommandLineToArgvW parsing rules. */
  std::wstring quoteCommandLineArgument(std::wstring_view argument);
} // namespace ao::winui
