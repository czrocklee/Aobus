// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace ao::utility
{
  std::string pathToUtf8(std::filesystem::path const& path);

  std::string pathToGenericUtf8(std::filesystem::path const& path);

  std::filesystem::path pathFromUtf8(std::string_view value);

  /**
   * Construct a path from the operating system's native filename code units.
   *
   * This is a byte-preserving boundary on POSIX and a wide-code-unit boundary
   * on Windows. It performs no Unicode decoding or normalization.
   */
  std::filesystem::path pathFromNative(std::basic_string_view<std::filesystem::path::value_type> value);
} // namespace ao::utility
