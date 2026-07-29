// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/utility/Path.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace ao::utility
{
  std::string pathToUtf8(std::filesystem::path const& path)
  {
    auto const value = path.u8string();
    return {value.begin(), value.end()};
  }

  std::string pathToGenericUtf8(std::filesystem::path const& path)
  {
    auto const value = path.generic_u8string();
    return {value.begin(), value.end()};
  }

  std::filesystem::path pathFromUtf8(std::string_view value)
  {
    auto const utf8 = std::u8string{value.begin(), value.end()};
    return std::filesystem::path{utf8};
  }
} // namespace ao::utility
