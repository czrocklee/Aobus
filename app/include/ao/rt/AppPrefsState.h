// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/OutputDeviceSelection.h>

#include <string>

namespace ao::rt
{
  struct AppPrefsState final
  {
    audio::OutputDeviceSelection preferredOutputSelection{};
    std::string lastLayoutPreset;
    std::string lastThemePreset;
  };

  struct AppSessionState final
  {
    std::string lastLibraryPath;
    audio::OutputDeviceSelection lastOutputSelection{};
  };
} // namespace ao::rt
