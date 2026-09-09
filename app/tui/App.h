// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/Log.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace ao::rt
{
  class CompletionAliasPolicy;
  class TextOrderingPolicy;
}

namespace ao::tui
{
  struct AppOptions final
  {
    std::filesystem::path libraryRoot{"."};
    std::filesystem::path databasePath{};
    std::filesystem::path configPath{};
    std::string coverArtMode{}; // Empty means use the persisted preference.
    rt::LogLevel logLevel = rt::LogLevel::Info;
  };

  std::int32_t run(AppOptions const& options);
} // namespace ao::tui
