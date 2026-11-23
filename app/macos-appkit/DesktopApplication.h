// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/desktop/LibrarySwitch.h>
#include <ao/i18n/MessageCatalog.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ao::appkit
{
  struct DesktopLaunch final
  {
    std::filesystem::path stateRoot;
    std::filesystem::path executable;
    std::optional<desktop::LibrarySwitchRequest> optRequest;
  };

  struct DesktopStartupArguments final
  {
    DesktopLaunch launch;
    bool successor = false;
    std::string scenario = "desktop";
  };

  // Takes arguments after argv[0]. Only the smoke entry point accepts --scenario.
  Result<DesktopStartupArguments> parseDesktopStartupArguments(std::span<std::string_view const> arguments,
                                                               bool allowScenario = false);

  // Call before creating NSApplication; desktop.plist owns native window state.
  void disableWindowRestoration();
  std::int32_t runDesktopApplication(DesktopLaunch launch, i18n::MessageCatalog catalog);
} // namespace ao::appkit
