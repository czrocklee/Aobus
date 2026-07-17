// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <string>

namespace ao::rt
{
  struct AppPrefsState final
  {
    std::string lastOutputBackendId;
    std::string lastOutputProfileId;
    std::string lastOutputDeviceId;
    std::string lastLayoutPreset;
    std::string lastThemePreset;
  };

  struct AppSessionState final
  {
    std::string lastLibraryPath;
    std::string lastOutputBackendId;
    std::string lastOutputProfileId;
    std::string lastOutputDeviceId;
  };
} // namespace ao::rt
