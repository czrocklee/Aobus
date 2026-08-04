// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <filesystem>
#include <optional>
#include <span>
#include <string_view>

namespace ao::winui
{
  inline constexpr std::string_view kLibraryRootOption = "--library-root";

  struct StartupOptions final
  {
    std::optional<std::filesystem::path> optLibraryRoot{};

    friend bool operator==(StartupOptions const&, StartupOptions const&) = default;
  };

  /**
   * @brief Parses the private WinUI startup arguments after argv[0].
   *
   * The native adapter owns `CommandLineToArgvW` and UTF-16-to-UTF-8
   * conversion. This parser deliberately receives already separated UTF-8
   * arguments so it can be tested without a Windows dependency.
   */
  Result<StartupOptions> parseStartupOptions(std::span<std::string_view const> arguments);
} // namespace ao::winui
