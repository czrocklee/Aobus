// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/Log.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace ao::tui
{
  struct Options final
  {
    std::filesystem::path libraryRoot{"."};
    std::filesystem::path databasePath{};
    std::filesystem::path configPath{};
    std::string coverArtMode{"auto"};
    rt::LogLevel logLevel = rt::LogLevel::Info;
  };

  std::int32_t run(Options const& options);
} // namespace ao::tui
