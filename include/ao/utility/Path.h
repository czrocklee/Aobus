// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace ao::utility
{
  inline std::string pathToUtf8(std::filesystem::path const& path)
  {
    auto const value = path.u8string();
    return {value.begin(), value.end()};
  }

  inline std::string pathToGenericUtf8(std::filesystem::path const& path)
  {
    auto const value = path.generic_u8string();
    return {value.begin(), value.end()};
  }

  inline std::filesystem::path pathFromUtf8(std::string_view const value)
  {
    auto const utf8 = std::u8string{value.begin(), value.end()};
    return std::filesystem::path{utf8};
  }
} // namespace ao::utility
