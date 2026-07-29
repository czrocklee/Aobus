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
} // namespace ao::utility
