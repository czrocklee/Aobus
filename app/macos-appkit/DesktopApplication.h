// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/desktop/LibrarySwitch.h>
#include <ao/i18n/MessageCatalog.h>

#include <cstdint>
#include <filesystem>
#include <optional>

namespace ao::appkit
{
  struct DesktopLaunch final
  {
    std::filesystem::path stateRoot;
    std::filesystem::path executable;
    std::optional<desktop::LibrarySwitchRequest> optRequest;
  };

  // Call before creating NSApplication; desktop.plist owns native window state.
  void disableWindowRestoration();
  std::int32_t runDesktopApplication(DesktopLaunch launch, i18n::MessageCatalog catalog);
} // namespace ao::appkit
