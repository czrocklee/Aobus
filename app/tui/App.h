// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/i18n/MessageCatalog.h>
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
    std::string coverArtMode{"auto"};
    rt::LogLevel logLevel = rt::LogLevel::Info;
  };

  std::int32_t run(AppOptions const& options,
                   i18n::MessageCatalog const& textCatalog,
                   rt::TextOrderingPolicy const& textOrderingPolicy,
                   rt::CompletionAliasPolicy const& completionAliasPolicy);
} // namespace ao::tui
