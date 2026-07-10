// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <string>
#include <string_view>

namespace ao::audio::backend::detail
{
  /// Converts a UTF-8 string to UTF-16 for Win32 wide-character APIs.
  std::wstring utf8ToWide(std::string_view text);

  /// Converts a UTF-16 string from Win32 wide-character APIs to UTF-8.
  std::string wideToUtf8(std::wstring_view text);
} // namespace ao::audio::backend::detail
